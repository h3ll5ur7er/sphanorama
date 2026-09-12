# 0053 — The harness reads its own datasets, against a fixture its own writer produced

**Status:** accepted

## Context

Phase 2 exits on a measured number: the median registration error over a synthetic dataset. Two
halves of the machinery to compute it already existed and had never met.

`tools/synth_dataset.py` renders the frames a phone would have captured from a known panorama and
writes the rotation each was taken at (ADR 0050). `core/test/support/rotation_scoring` turns a set
of estimated rotations into an angular error against truth, with the global gauge removed first
(ADR 0049). Between them sits nothing: no C++ in this repository read a dataset.

The evidence, stated accurately this time. `git grep -li dataset` across `core/test` at the commit
before this work finds **six** files — `rotation_scoring.{h,cpp}` and its test, plus
`frame_quality_engine_test.cpp`, `camera_model_test.cpp` and `quaternion_test.cpp` — and not one of
them reads a dataset off disk. Only `frame_quality_engine_test.cpp` uses the word about something
else; `camera_model_test.cpp` and `quaternion_test.cpp` are squarely *about* the generator — they
pin its projection and its quaternions against the core's, at decimals neither side derived — which
is worth knowing, because they pin the generator's arithmetic to the core's at shared decimals.
(They are **not** drift detectors — neither reads a dataset — and `docs/02` spent a revision saying
they were.)
Two versions of this sentence were wrong before this one: the first said "the renderer's own tests
and the scorer's", tidier than what the command prints, and the second said the last three use the
word about something else. Reviewers ran it both times.

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

**JSON comes from `cv::FileStorage` rather than a parser written here.** No *C++* in this repository
parses JSON — the session document has its own text format — so the choice was between a dependency
already present and a new hand-rolled parser in the one language that had no reader.

That sentence first read "nothing in this repository parses JSON", which a reviewer showed is plainly
false: `shell/src/access/spill-host.ts` parses the spill index on the product path, and
`tools/check_dist_fresh.mjs` and `tools/test_synth_dataset.py` both parse JSON in the tooling —
`CMakePresets.json` and `compile_commands.json` for the first, `truth.json` for the second. (An
earlier version of this sentence said "both parse **it**", whose only antecedent was the spill index,
which neither of them touches. A correction paragraph naming counterexamples, itself wrong about
which file reads what.) The
claim that carried the argument was always the narrower one, and overstating it made the decision
look better supported than it was. Every consumer of this loader is on the registration
path, which exists only where OpenCV does (ADR 0052), so the dependency costs nothing that was not
already paid. It was measured against the real `truth.json` before being chosen, not assumed: it
reads the nested `intrinsics`, the `frames` sequence and each `rotation`.

**The rotation convention is checked, not assumed.** `truth.json` carries a `convention` block —
camera space, image space, principal point, equirectangular layout, rotation, pixel encoding — and
the loader refuses any dataset whose `convention.rotation` is not the exact string it was written
against. By exact text, because the field is prose and prose that changed meaning is precisely what a
reader must not accept quietly; rewording it is a decision, so it fails in the loader and again in
`tools/test_synth_dataset.py` beside the writer.

This is checked because its violation is the one that leaves no trace. A dataset written the other
way round loads cleanly, scores cleanly, and every number it produces is wrong by an inverse — which
is the exact failure mode `docs/00-principles.md` opens with, a rotation slightly wrong looking fine
until the seam. The block had been written by the generator since ADR 0050 and read by nothing; it
took a reviewer asking for the *set* of `truth.json`'s top-level keys rather than the presence of the
two the loader wanted to notice that a third had been there all along. The remaining entries are
read past and not checked here — *read past*, not carried: nothing of the `convention` block reaches
`SyntheticDataset`, whose two members are `lens` and `frames`. This is the same verb round 7
corrected in `docs/02` and 126 lines below in this file, and it survived here because the sentence
around it is about the *file's* keys, where "carried" is true. The first
thing to compute on those pixels owes `pixel_encoding` the same treatment, since the bytes are signed
components rather than the unsigned [0, 1] a reader would assume.

**A small dataset is committed, and read by the loader's tests.** `core/test/data/synthetic-ring-4`
is four 48x36 frames plus the `truth.json` that describes them: **22,570 bytes of content** —
20,788 of frames at 5,197 bytes each, and 1,782 of JSON. Not "on disk": what it occupies on disk is
40 KiB, which is the whole subject of the paragraph below, and writing "on disk" against the content
figure is the same mislabel that paragraph exists to record.

An earlier version of this line attached that whole total to "four 48x36 frames", which is 1,782
bytes more than the frames are. The sizes above are file sizes — the only figure here that means
something without a footnote, for the reason the paragraph below spends four sentences on.

This is a deliberate exception to "datasets are regenerated rather than committed", and the
distinction is what the file is *for*. A measurement dataset is large, regenerated, and its pixel
values are the point. This one is a **format** fixture: it exists so the loader is read against bytes
its own writer produced, rather than against the author's idea of the format. Twenty-two kilobytes is
a cheap price for removing that class of error.

***And the first version of this ADR claimed more for it than it earns.*** It said the fixture caught
an off-by-one in the header reader "which a hand-written fixture would not have", and a reviewer
disproved that directly: a hand-typed 4x3 fixture dies to the same sabotage, in the same run. The bug
was in the reader, not in the author's understanding of the format, so the one thing a real fixture
uniquely guards against is not what caught it. What remains true is narrower and worth keeping: a
hand-written fixture is written to match whatever the author believes the format is, so it cannot
catch a belief that is wrong — and no evidence here shows that risk being realised.

The first version of this paragraph said forty, and made the cost argument on that number. It was
`du`'s block figure reported as the size of the bytes — four 5,197-byte files and one 1,782-byte file
rounded up to whole 4 KiB blocks is 36 KiB, and the fortieth kilobyte is the directory entry `du`
also counts. The content is **1.8x smaller than `du` reported**, and git stores the five objects in 13,589 bytes
(13.3 KiB) — **1.7x below the content figure, 3.0x below `du`'s**. Both ratios are written out
because "3.0x smaller again" reads as a further reduction from 22,570, which would be 1.7. So the argument was sound
and the evidence for it was inflated, which is the combination that is easiest to let through.

A second reviewer then found that the correction itself needed correcting on all three of its
numbers: it rounded 13.27 KiB to 13.2, it described the block figure as the disk figure and the
content figure as "on disk" — the labels swapped, in a paragraph whose entire subject is a number
reported as something it was not — and it attributed 40 KiB to five files that account for 36.
Measuring the magnitude and not the referent is the shape of every wrong number on this branch.
`tools/synth_dataset.py`'s own docstring carried the uncommitted-datasets claim too, and now records
this exception.

**The format is pinned from the writer's side too.** `tools/test_synth_dataset.py` asserts the header
shape and the `truth.json` keys the C++ loader reads, naming that consumer. The format is written
down in two languages whether anybody likes it or not; what this buys is that a change fails in the
file that made it, with a message about the schema, rather than in a C++ test whose message is about
a frame.

**Three test translation units take `-fexceptions`**, extending ADR 0052's boundary to test support for
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
  would once have kept the C++ tests green on a format nothing writes any more. **That half is
  closed**: `test_the_committed_fixture_is_still_this_generator_s_output` re-renders the ring and
  compares it byte for byte, so a generator change fails on the Python side and names the fixture as
  the thing to regenerate. What is still open is the narrower thing — the C++ never reads a freshly
  generated dataset, only a committed one. Running the two together at
  test time is the stronger check and it is not done here: it would make the native test build depend
  on `uv` and numpy, which is a real coupling to buy with a real argument, and the argument is better
  made when something actually measures accuracy in CI.
- **The fixture's pixels are not asserted anywhere.** Deliberately: a pixel value that shifts when
  numpy changes its rounding is not a contract, and a test that went red on an unrelated dependency
  bump would be noise. What the fixture pins is shape, and the loader's own tests read its pixels
  back through an independent parser rather than trusting them.
- **The "gives every frame back" promise gets its own test binary, because nothing else could
  ask.** (Five places, this one included, used to quote it as "a refusal allocates nothing" — a
  sentence the header has never contained, and a stronger claim than the code makes: a refusal
  allocates and then returns.) `sphanorama_dataset_alloc_test` replaces global `operator new` and sweeps a failure across
  every allocation of a full load, checking the store's totals return to baseline each time. It is a
  separate executable for two reasons: replacing `operator new` is not something to do to a process
  seven hundred other tests share, and gtest allocates while it runs, which would move the sweep's
  own counter. Round 2 found two windows with exactly this instrument — one of them inside round 1's
  fix for the same promise — and three reviewers each built it by hand before it was kept.
- **Leak detection stays on for that binary, with one suppression, and the reasoning is a
  measurement.** Left unsuppressed, the sweep reports its own totals clean while LeakSanitizer finds
  frames unfreed: the throw lands on the red-black-tree node allocation inside `entries_.emplace` in
  `MemoryFrameStoreAccess::Allocate`, and the block orphaned is the local `Entry`'s pixel buffer
  allocated a line earlier. That function is compiled `-fno-exceptions` (ADR 0012), so GCC emits no
  cleanup landing pads and the throw never runs its destructor. Making an allocation fail *inside*
  code that has opted out of exceptions is not something the real program can do — there the same
  failure terminates — so the leak is the instrument's rather than the loader's, and
  `support/dataset_alloc_test.lsan-suppressions` names the function they come from. **Not one
  frame**: one per sweep point that fails an allocation inside `Allocate`, in both passes — 138,240
  bytes in 20 allocations when measured, though the suppression file explains why that figure is an
  illustration rather than a number to maintain. This sentence said "that one frame" until round 7,
  four lines above the paragraph below that corrects the same mistake.

  Two corrections are recorded here rather than smoothed over, because both are about how the first
  version was *measured*. It said "one 6,912-byte frame … allocation 82 of 119", read off a bisect
  against an earlier state of the file — the committed binary strands a frame per dataset frame at
  several points — and it named the tree node as the leaked block, conflating where the throw fires
  with what it orphans. And it disabled leak detection for the whole binary, which is process-wide
  on the only test that reaches the loader's exception arms: a leak planted anywhere in it would
  have been green. Reviewers found all three, and the narrow suppression is theirs.

- **Three files under `core/test` now carry `-fexceptions`**, where ADR 0052 had said
  `feature_registration_engine.cpp` alone carried it "and no other translation unit". That sentence
  was true of `core/src` and is now qualified there; the three added here are test support and ship
  nowhere. Being test support bought no leniency in practice — both of round 2's worst findings were
  in this loader's own exception boundary.
- **The format is spelled in two languages, so `docs/02-volatility-map.md`'s axis has two owners.**
  A change to the dataset format has to be made in `tools/synth_dataset.py` and again here. That cost
  is the point rather than an oversight: a loader checked only against bytes its own author wrote is
  checked against its author's idea of the format. Both sides have a test that fails when the two
  drift, and `docs/02-volatility-map.md` names them — after three revisions that named tests which
  could not, because the question is what a test *reads* and the wrong answers named tests for what
  they are *about*. **Not both at once**, and a reviewer had to point that out too: change the
  generator and the Python side goes red alone, because the C++ reads the committed fixture and the
  committed fixture has not moved. The C++ half is reached only once the fixture is regenerated,
  which is what `test_the_committed_fixture_is_still_this_generator_s_output` forces a change to do.
  The two fire in sequence, not in parallel, and the consequence four bullets up says the same thing
  from the other end. And the C++ half covers only what the loader reads: the `convention` entries
  it deliberately reads past without checking — pixel encoding among them — are held on the Python
  side alone until something computes on those pixels. (Reads past rather than carries: nothing of
  the `convention` block survives into `SyntheticDataset`, whose two members are `lens` and
  `frames`.)
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
skipped check.

Two corrections a reviewer is owed here. The coupling argument is about *build* time and was written
as though it settled *test* time as well, which it does not — and `tools/gate.sh` already refuses to
run without `uv` and already invokes `--group datasets`, so the gate specifically pays that cost
today. A third option was never weighed: a ctest fixture that generates a dataset when the tooling is
present and skips when it is not. That is probably the right answer, and it is left out of this
increment rather than argued away — the honest reason is scope, not cost. Named in the consequences
above as the thing to revisit when CI measures accuracy.

***Asserting the fixture is byte-identical to a fresh render — rejected here, and then adopted.***
The original argument was that it would go red on a numpy upgrade that changed a rounding mode,
which is an unrelated event, and that the structural assertions catch what matters to a reader of
the format.

The second half turned out to be false, and a reviewer showed how: the structural assertions say
nothing about the file naming, the pixel encoding, or the pose the ring starts at. Change any of
those and the C++ suite keeps passing against bytes no writer produces any more — which is the class
of error this fixture was committed to remove, reappearing one level up. So the check is in, as
`test_the_committed_fixture_is_still_this_generator_s_output`, and the cost named above is now an
accepted cost rather than a reason: a numpy change that moves a pixel will turn it red, and the
failure message says to regenerate the fixture rather than to edit the expectation. It runs in
about half a second — 0.563 s by `unittest`'s own clock, 0.35 s to 0.84 s wall including `uv`'s
startup. This line said "a fifth of a second" until a reviewer timed it.

This paragraph is kept in `Rejected` rather than deleted, because what was thought at the time is
the point of the section — but a reviewer had to point out that the branch had implemented the thing
this section rejects, in a commit that edited this very file and did not touch these lines.
