# 0053 — The harness reads its own datasets, against a fixture its own writer produced

**Status:** accepted

## Context

Phase 2 exits on a measured number: the median registration error over a synthetic dataset. Two
halves of the machinery to compute it already existed and had never met.

`tools/synth_dataset.py` renders the frames a phone would have captured from a known panorama and
writes the rotation each was taken at (ADR 0050). `core/test/support/rotation_scoring` turns a set
of estimated rotations into an angular error against truth, with the global gauge removed first
(ADR 0049). Between them sits nothing: no C++ in this repository read a dataset. `grep` for the word
across `core/test` found the renderer's own tests and the scorer's, and no consumer.

So the accuracy number Phase 2 is defined by could not be computed at all, and `EstimatePairwise` —
the next increment, and the first whose correctness is invisible to the eye — would have had nowhere
to be measured.

## Decision

**`core/test/support/synthetic_dataset` reads a dataset into a frame store**, handing back the lens
it was rendered through and, per frame, a `FrameRef` and the rotation `truth.json` records.

Three things it deliberately does not do.

***It does not re-implement the lens.*** ADR 0050 keeps the generator's projection independent of
`utilities/camera_model` precisely so a shared error cannot cancel; this reads the recorded
`intrinsics` and hands them over unexamined. A loader that "checked" them against our own projection
would re-couple exactly what that ADR separated.

***It does not tidy the rotations.*** A quaternion and its negation are the same rotation, and a ring
of four frames emits a negative scalar part on the last one. The loader records what the file spells.
`ScoreRotations` handles the double cover itself, and a loader normalising first would be doing
unasked-for work on the one field every accuracy number is compared against.

***It does not accept a file that is nearly right.*** A header disagreeing with the recorded lens, a
payload shorter *or longer* than the header accounts for, a Netpbm that is not `P6`, a maximum that
is not 255 — each is a refusal. The longer case matters as much as the shorter: a file with bytes to
spare is not the frame its header describes, and reading the first `w * h * 3` of it succeeds
silently while quietly picking a different picture.

**JSON comes from `cv::FileStorage` rather than a parser written here.** Nothing in this repository
parses JSON — the session document has its own text format — so the choice was between a dependency
already present and a new hand-rolled parser. Every consumer of this loader is on the registration
path, which exists only where OpenCV does (ADR 0052), so the dependency costs nothing that was not
already paid. It was measured against the real `truth.json` before being chosen, not assumed: it
reads the nested `intrinsics`, the `frames` sequence and each `rotation`.

**A small dataset is committed, and read by the loader's tests.** `core/test/data/synthetic-ring-4`
is four 48x36 frames, 40 KB, written by the real generator.

This is a deliberate exception to "datasets are regenerated rather than committed", and the
distinction is what the file is *for*. A measurement dataset is large, regenerated, and its pixel
values are the point. This one is a **format** fixture: it exists so the loader is read against bytes
its own writer produced, rather than against the author's idea of the format, which is the one thing
a hand-written fixture cannot do. Forty kilobytes is a cheap price for removing that class of error.

**The format is pinned from the writer's side too.** `tools/test_synth_dataset.py` asserts the header
shape and the `truth.json` keys the C++ loader reads, naming that consumer. The format is written
down in two languages whether anybody likes it or not; what this buys is that a change fails in the
file that made it, with a message about the schema, rather than in a C++ test whose message is about
a frame.

**Two test translation units take `-fexceptions`**, extending ADR 0052's boundary to test support for
the same reason: `cv::Exception` is how OpenCV reports ordinary failure, and the loader converts it
to a `Result` at its own edge. The test file takes it too, because it copies and deletes directories
to build the damaged datasets its refusal cases need and `std::filesystem` reports those failures by
throwing — which under `-fno-exceptions` is a terminate that reads as a crash rather than as the
fixture problem it is.

## Consequences

- **The accuracy number becomes computable, and is still not computed.** This closes the gap; it does
  not produce a measurement. Phase 2's threshold stays blank until `EstimatePairwise` exists and has
  been run, which is the order the roadmap argues for: a number chosen before anything can produce
  one is a number the implementation gets tuned to.
- **The loader is checked against a committed fixture, not a freshly generated one.** If the
  generator's output shape changed, the Python cases above would fail — but the committed fixture
  would keep the C++ tests green on a format nothing writes any more. Running the two together at
  test time is the stronger check and it is not done here: it would make the native test build depend
  on `uv` and numpy, which is a real coupling to buy with a real argument, and the argument is better
  made when something actually measures accuracy in CI.
- **The fixture's pixels are not asserted anywhere.** Deliberately: a pixel value that shifts when
  numpy changes its rounding is not a contract, and a test that went red on an unrelated dependency
  bump would be noise. What the fixture pins is shape, and the loader's own tests read its pixels
  back through an independent parser rather than trusting them.
- **A dataset costs the heap what its frames cost.** Four 48x36 frames are nothing; sixty frames of a
  real capture are not. The frames belong to the caller, who must `Forget` each — the same rule
  `ExtractFeatures` states, and for the same reason: a harness leaking a dataset per run would
  exhaust the store somewhere in the middle of a measurement and report that instead.

## Rejected

***A JSON parser written here, to avoid leaning on OpenCV.*** Attractive for about a minute: it would
keep the loader usable in a build without OpenCV. But nothing in such a build has anything to do with
a dataset — registration is the only consumer and it does not exist there — so the independence buys
nothing and costs a second parser to get wrong. The subset would have grown the first time the schema
did.

***Generating the fixture at build time from `synth_dataset.py`.*** The strongest version of the
format check, and rejected for this increment on coupling: it would make `cmake --build` of the
native tests require `uv` and numpy, so a contributor without them gets a broken build rather than a
skipped check. Named in the consequences above as the thing to revisit when CI measures accuracy.

***Asserting the fixture is byte-identical to a fresh render.*** Would catch every drift, including
the numeric ones — and would go red on a numpy upgrade that changed a rounding mode, which is an
unrelated event. The structural assertions catch the changes that matter to a reader of the format.
