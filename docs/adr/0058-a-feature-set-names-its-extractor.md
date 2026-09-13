# 0058 — A feature set names its extractor, opaquely

**Status:** accepted

## Context

`FeatureSet` carried a frame, a count, and two `FrameRef`s. Nothing in it said what produced the
descriptors, so `EstimatePairwise` could not tell its own sets from another extractor's. It compared
descriptor *widths* instead, and a comment in the engine admitted that is a proxy rather than a
check.

The proxy is blind in the case that matters. It compares the two sets to each other, so when both
come from one foreign extractor they agree and neither is ever compared to the engine reading them.
A reviewer drove all nine (writing detector, reading engine) pairs on a rendered ring: six answer,
and three come back `accepted = true` from a pair matched under the wrong metric — SIFT's 512-byte
float rows read as 512 Hamming bytes give 30 correspondences where the right metric finds 178. Not
memory-unsafe; a wrong answer reported confidently, which is what the whole `accepted` apparatus
exists to prevent.

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
- **The identity is unique per detector, not per implementation.** Two different
  `IRegistrationEngine`s would both stamp 1 for their first detector, so a set from one would be
  accepted by the other. There is one implementation, so this costs nothing today; making it a
  global registry before there is a second implementation would be inventing a problem.
- `FeatureSet` crosses the generated TypeScript mirror, so the field appears there. Engines never
  cross the WASM boundary, so nothing in the shell reads it — the mirror carries it because the
  struct is mirrored, not because anyone on that side has a use for it.

## Rejected alternative

**Keep the width check and sharpen it** — compare the descriptor width against what this engine's
detector produces, rather than the two sets against each other. That closes the measured hole with
no contract change, and it is genuinely tempting.

It loses because width is not identity. Two detectors with the same descriptor width and different
metrics would pass, and the check would then be asserting something it cannot know while reading as
though it can — which is the exact defect being fixed, moved one level down. It also keeps the
engine inferring a fact about a set's provenance from its shape, when the producer could simply have
said so.
