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

An earlier draft of this section said six answer and three are accepted, with 30 correspondences
against 178. Those figures named no frame pair and do not reproduce on this one; they are replaced
here rather than retracted in an ADR of their own (ADR 0057's rule) because they were never
published.

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

- The refusal moved *earlier*: a foreign pair is now refused before any `Pin`, where it used to be
  refused after four. That is better and it invalidated a test's arrangement —
  `EstimatePairwiseForgetsNoneOfTheFourFramesItIsHanded` needed a post-pin refusal and had been
  using a foreign detector's set to get one. It uses a doctored stride now: same extractor, a row
  width the frame does not have.
- The width check stays. It is no longer reachable by a foreign extractor, but a caller can still
  hand over a set whose stride it set itself, and that is what the bounds-guard test drives.
- **The identity is unique per detector, not per implementation, and it says nothing about which
  store the frames live in.** Two different `IRegistrationEngine`s would both stamp 1 for their
  first detector, so a set from one would be accepted by the other. And `MemoryFrameStoreAccess`
  numbers its frames from 1 per instance, so a set made over one store passes the guard of an engine
  over another and its `FrameRef`s then resolve against frames it has never seen. Both are
  unreachable today — there is one implementation, no composition root wires an `IRegistrationEngine`
  at all, and every test uses one store — and both are reasons to read this field as one half of
  provenance rather than the whole of it. A global registry or a store identity before there is a
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
