# 0058 — A feature set names its extractor, opaquely

**Status:** accepted

## Context

`FeatureSet` carried a frame, a count, and two `FrameRef`s. Nothing in it said what produced the
descriptors, so `EstimatePairwise` could not tell its own sets from another extractor's. It compared
descriptor *widths* instead, and a comment in the engine admitted that is a proxy rather than a
check.

The proxy is blind in the case that matters. It compares the two sets to each other, so when both
come from one foreign extractor they agree and neither is ever compared to the engine reading them.

**Measured on an arrangement anyone can re-run**: the twelve-frame ring `tools/synth_dataset.py`
renders at 640x480, frames 0 and 1, the prior nudged three degrees about x — the accuracy suite's
own arrangement — with each (writing detector, reading engine) pair forced past the provenance guard
so that what is measured is the behaviour before it.

| writing detector → reading engine | outcome | inliers / correspondences |
| --- | --- | --- |
| ORB → ORB, AKAZE or SIFT | RegistrationFailed | the best hypothesis had 0, 0 and 2 |
| AKAZE → ORB | accepted | 42 / 202 |
| AKAZE → AKAZE | accepted | 42 / 202 |
| AKAZE → SIFT | InvalidArgument: rows are not a whole number of elements wide | – |
| SIFT → ORB | **accepted** | 13 / 18 |
| SIFT → AKAZE | **accepted** | 13 / 18 |
| SIFT → SIFT | accepted | 60 / 181 |

Five answer, five are accepted, and **two** of those are matched under the wrong metric: SIFT's
512-byte float rows read as 512 Hamming bytes come back accepted on 13 of 18 correspondences where
the right metric finds 60 of 181. Not memory-unsafe; a wrong answer reported confidently, which is
what the whole `accepted` apparatus exists to prevent.

**At most three of the nine can be wrong that way at all**, which bounds the defect and is the
reason to give the whole table rather than the headline. `DescriptorType` answers `CV_8U` for both
ORB and AKAZE and the norm is `NORM_HAMMING` for both, so AKAZE → ORB is bit-identical to the
diagonal — 42 of 202 twice — and ORB → AKAZE fails identically to ORB → ORB. The six cross pairs are
three that cross the metric, two that are clones of the diagonal, and one the divisibility check
refuses.

**Retracting the earlier figures, which were published after all.** An earlier draft of this section
said six answer and three are accepted, with 30 correspondences against 178. Those figures named no
frame pair and do not reproduce on this one. This paragraph previously excused itself from ADR 0057's
rule on the grounds that they "were never published" — which was wrong: they are on `main` verbatim
in `feature_registration_engine.cpp`, in a merged source comment that this change deletes.
`docs/00-principles.md` says "a measured figure this repository has published" and draws no line
between a figure published in `docs/` and one published in code; ADR 0057's own rejected-alternative
section notes the carrier need not be an ADR, since there the carrier was the roadmap.

So this *is* the retraction, and it is recorded here rather than in an ADR of its own because this is
the ADR that replaces them — which is the shape ADR 0057 describes, not an exemption from it. The
figures above are the ones to believe; six-answer/three-accepted and 30-against-178 are withdrawn.

Two other things made this worth a contract change rather than a sharper guard:

- The width proxy is good only because three detectors happen to have three widths. A fourth with
  32-byte rows, or a caller that sets a stride itself, falsifies it with no code changing.
- Four places in the header and the engine described the guard as a detector check. Three were
  corrected to say "widths" and the fourth was missed, which is the argument for making the code do
  what the sentence wanted rather than for editing sentences.

## Decision

**`FeatureSet` gains `int32_t extractor`, an opaque producer identity. Zero means unstamped, and the
only operation the contract defines on it is equality.**

`ExtractFeatures` stamps it. `EstimatePairwise` refuses, before pinning anything, unless both sets
carry this engine's own value — which also refuses an unstamped set, since no extractor writes zero.

**It is not a detector enum**, and that is the decision rather than an implementation detail. A
caller never chooses the detector: it is construction-time state of one implementation. Putting
`FeatureDetector` in the contract would publish ORB, AKAZE and SIFT as part of `IRegistrationEngine`,
and an implementation with no notion of them — a learned matcher, say — would then be satisfying an
interface that names a taxonomy it does not have. Every other closed enum in `types.h` is something
a caller picks or branches on. This is neither.

## Consequences

- **Two pairings that were previously correct are now refused, and that is a price rather than a
  fix.** ORB and AKAZE share `CV_8U` and `NORM_HAMMING`, so before the stamp existed each read the
  other's rows under the right metric and answered sensibly. The stamp cannot tell those two apart
  from the pairing that is genuinely wrong, so it refuses all six cross pairs including the two that
  worked. Accepted because a caller never chooses the detector — nothing in this repository pairs
  across engines on purpose — and because a guard that is right for the wrong reason on a third of
  its cases is worse than one that is uniformly strict. This was recorded only inside a *rejected*
  alternative, as a benefit of the option that lost, while the test asserting it cited this section.

- The refusal moved *earlier*: a foreign pair is now refused before any `Pin`, where it used to be
  refused after four. That is better and it invalidated a test's arrangement —
  `EstimatePairwiseForgetsNoneOfTheFourFramesItIsHanded` needed a post-pin refusal and had been
  using a foreign detector's set to get one. It uses a doctored stride now: same extractor, a row
  width the frame does not have.
- The width check stays. It is no longer reachable by a foreign extractor, but a caller can still
  hand over a set whose stride it set itself. **Which test reaches it is worth naming precisely,
  because an earlier version of this line named the wrong one.** Every case in
  `EveryBoundsGuardRefusesRatherThanReadingPastTheFrame` asserts a message from a guard that fires
  strictly earlier — a stride floor, a row count, element divisibility — so none of them arrives at
  the comparison between the two sets. The case that does is
  `EstimatePairwiseForgetsNoneOfTheFourFramesItIsHanded`'s halved descriptor stride, and that test
  deliberately asserts only `InvalidArgument` and a pin count rather than which guard refused.
- **The identity is unique per detector, not per implementation, and it says nothing about which
  store the frames live in.** Two different `IRegistrationEngine`s would both stamp 1 for their
  first detector, so a set from one would be accepted by the other. And `MemoryFrameStoreAccess`
  numbers its frames from 1 per instance, so a set made over one store passes the guard of an engine
  over another and its `FrameRef`s then resolve against frames it has never seen. Both are
  unreachable today, though **not for the reason an earlier version of this sentence gave**: there are
  two implementations — `NullRegistrationEngine` as well as this one — and `bridge/runtime.h` does
  wire one, unconditionally. ADR 0052's Decision section corrected this exact sentence once already.
  What actually holds is narrower and is enough: the null engine never returns a `FeatureSet`, so no
  set it produced can be handed to anyone, nothing outside the tests constructs the OpenCV engine,
  and every test uses one store. Both are still reasons to read this field as one half of provenance
  rather than the whole of it. A global registry or a store identity before there is a
  second implementation would be inventing a problem.
- `FeatureSet` crosses the generated TypeScript mirror, so the field appears there. Engines never
  cross the WASM boundary, so nothing in the shell reads it — the mirror carries it because the
  struct is mirrored, not because anyone on that side has a use for it.

## Rejected alternatives

**Carry the descriptor type instead of a producer identity**, which is the option this engine's own
code asks for: `DescriptorType`'s docblock has said since it was written that the type is decided
where the descriptors are written and re-derived when they are read, and that a field on `FeatureSet`
is what removes the copy. It would close the measured hole — a `CV_32F` set handed to a `CV_8U`
engine is exactly the SIFT-read-as-Hamming case — remove that second copy, and *allow* the two cross
pairs the table above shows are harmless.

It loses on what it identifies. The type names the metric, not the producer, so two detectors
sharing a metric stay indistinguishable — which is the same objection as "width is not identity",
one level up, and ORB and AKAZE are that pair today. And the contract cannot say `CV_8U`: naming
OpenCV's type codes in `types.h` publishes one library's taxonomy through an interface that a
learned matcher is supposed to be able to satisfy, so it would need an abstraction of its own, which
is a second decision to pay for a strictly weaker check. The copy in `DescriptorType` stays, as a
cost recorded there.

**Keep the width check and sharpen it** — compare the descriptor width against what this engine's
detector produces, rather than the two sets against each other. That closes the measured hole with
no contract change, and it is genuinely tempting.

It loses because width is not identity. Two detectors with the same descriptor width and different
metrics would pass, and the check would then be asserting something it cannot know while reading as
though it can — which is the exact defect being fixed, moved one level down. It also keeps the
engine inferring a fact about a set's provenance from its shape, when the producer could simply have
said so.
