# 0061 — The half-pixel convention gap is measured, and kept until its cost can be paid

**Status:** accepted

## Context

`camera_model.h` puts the origin at the **top-left corner of the image**, so a pixel centre sits at
a half-integer: pixel index `i` has camera-model coordinate `i + 0.5`. OpenCV's keypoint detectors
use the other convention — a pixel's centre is at an integer. The header has stated the gap since it
was written.

`ReadBearings` hands keypoint coordinates to `Unproject` unchanged. So every bearing this engine
computes is the bearing of a point **translated by (-0.5, -0.5) pixels**: a constant offset in the
image plane, absorbed by the optical centre rather than the focal length, and not a radial one. The
`Bearing` docblock has called that "a systematic error in the measurement" for several review rounds
and deferred the fix to an ADR, without ever saying how large it is or which way correcting it moves
the numbers.

Both are now measured, and the second is the surprise.

**The correction is geometrically exact.** Correspondences built the way `tools/synth_dataset.py`
builds a frame — pixel index `i` sampled at `i + 0.5` — and fitted back by Kabsch through the
engine's convention shifted by `s`, on the accuracy dataset's own lens (640x480, `fx` 492.757,
30-degree ring step):

| shift | rotation error | worst residual |
| ----- | -------------- | -------------- |
| -0.50 | 0.020582° | 0.6614 px |
| -0.25 | 0.015439° | 0.4959 px |
| +0.00 | **0.010294°** | 0.3305 px — what the engine does today |
| +0.25 | 0.005148° | 0.1652 px |
| +0.50 | **0.000000°** | 0.0000 px — the correction |

Linear in the shift, exactly zero at `+0.5`. Reproduced independently on the committed
`synthetic-ring-4` lens (48x36, `fx` 36.96, a 7-degree rotation): 0.0549° at `+0.0`, 0.1104° at
`-0.5` — twice, as a linear term must be — and a 6.1e-16 residual at `+0.5`. A smaller lens gives a
larger angle for the same half pixel, which is what a fixed image-plane offset should do. Two
lenses, two rigs, one geometry.

**And applying it turns the suite red.** Patching that one line and re-running
`EveryDetector/Accuracy.*` against the same rendered ring:

| detector | median today → corrected | max today → corrected |
| -------- | ------------------------ | --------------------- |
| ORB | 0.1009 → **0.2929** | 0.1551 → 0.4713 |
| AKAZE | 0.0612 → 0.0562 | 0.1330 → **0.2566** |
| SIFT | 0.0239 → **0.0626** | 0.0435 → 0.1062 |

ORB fails `EXPECT_LT(score.medianDeg, 0.2)` outright. A sweep of the shift puts SIFT's median
minimum at exactly `s = 0` — where the *model* is worst.

Five of the six figures get worse and one does not: AKAZE's median improves by 0.005 while its worst
frame nearly doubles. That one is the reason to read the max column as well as the median, and the
reason this ADR does not say "every figure gets worse".

**So the published table is green partly by cancellation.** This model error runs against whatever
sub-pixel localisation bias the three detectors carry, and removing one of the two leaves the other.
OpenCV says as much in passing while the suite runs — *"SIFT_Impl precise upscale disabled, this is
now deprecated as it was found to induce a location bias"*. The chaining-and-gauge-removal absorbs a
further part of a per-step conjugation, which is why the per-pair 0.0103° and the per-chain medians
are not the same quantity and cannot be subtracted from one another.

## Decision

**Keep the convention gap for now. Publish its size and its direction. Name what a correction has to
move, so the next person starts from a measurement rather than a sign.**

Concretely:

1. `ReadBearings` is unchanged. The line stays `Pixel{xy[0], xy[1]}`.
2. The `Bearing` docblock carries both tables above and the sentence that matters operationally:
   **a red ORB after the correction is not an estimator regression.** It is the engine becoming
   honest about its geometry while the bound above it still encodes the cancelled number.
3. `docs/06-roadmap.md` keeps its live table and its existing paragraph saying the bearings are half
   a pixel out, and now points here for what correcting them costs.
4. When the correction is made it moves, **in one commit**: the `+ 0.5` in `ReadBearings`; the
   `EXPECT_LT` bound in `registration_accuracy_test.cpp`; the accuracy table in the roadmap;
   `CLAUDE.md`'s three medians; and ADR 0059, which keeps the figures it published under ADR 0057's
   convention. Any subset of those is a tree that contradicts itself.

**Why not simply correct it now.** Because the honest correction is not one line, and the one-line
version is worse than the gap. Moving `ReadBearings` alone leaves a red build. Moving the bound with
it publishes a *larger* error as the project's accuracy while claiming an improvement, with no
explanation available to a reader — the improvement is in the model, the number is in the
measurement, and they move opposite ways. What makes that defensible is a measurement that separates
the model error from the detector bias, and this repository cannot take it yet: it needs a dataset
whose keypoint localisation is known rather than inferred, which is a harness increment of its own
alongside the noise, blur, rolling-shutter and distortion increments ADR 0050 names.

The gap is a *known, bounded, constant* error today. That is a better state than an unexplained
number, and much better than a red build.

## Consequences

**The accuracy table is a comparison between detectors on equal terms, and is not yet a statement of
how well this estimator locates a rotation.** All three detectors read bearings through the same
half-pixel offset, so the ranking is unaffected and the absolute figures are not clean. Anywhere
those medians are quoted as "how accurate registration is", this ADR is the qualification.

**A future harness increment can settle it.** Rendering with distortion, and with known sub-pixel
feature positions, makes the model error separable from the detector's. Until then the two are
measured together and only their sum is observable.

**`camera_model`'s convention is not the thing that is wrong.** Corner-origin with half-integer
centres is the convention the renderer and the model already share, and the dataset is rendered
through it. The gap is entirely at the boundary where OpenCV's keypoints arrive, which is one line
in one function — the fix is small and the *consequences* are not.

**This ADR is itself a measurement and inherits ADR 0057's rule.** If the figures above are ever
found to have been measured wrongly, they are retracted here rather than quietly replaced.

**The bound stays at 0.2 and the threshold at 0.5 degrees.** Both were written against the numbers
the engine actually produces. Correcting the geometry without re-deriving them would leave two
constants tuned to a world that no longer exists, which is the failure ADR 0057 records.

## Rejected alternatives

***Correct `ReadBearings` and relax the bound to match.*** One line plus one constant, and the build
is green again. Rejected because the resulting number is unexplainable: the project would publish a
*worse* accuracy figure immediately after a change described as a correctness fix, and nothing in
the tree would say why. It also spends the bound — the one gate standing between this engine and a
silent regression — to pay for a change that was supposed to improve it. A bound that moves whenever
it is inconvenient is not a bound.

***Correct `ReadBearings` and leave the bound, accepting a red build until the estimator improves.***
Honest, and unusable. A red `main` is a signal nobody can read: the next real regression arrives into
a suite that is already failing, and the failure that matters is invisible among the one that is
expected. This project's whole argument for a measured bound is that it distinguishes a regression
from a re-tuning, and a permanently red test cannot.

***Split the difference — apply a half-pixel correction to the renderer instead, so the two
conventions meet in the middle.*** Rejected because it makes the *dataset* wrong in order to make
the *engine* look right. The renderer's convention matches `camera_model`'s and is correct; moving it
would mean every dataset this repository produces encodes a compensating error, which is exactly the
shared-error failure ADR 0050 exists to prevent — a wrong convention that renders wrong, registers
wrong in the compensating way, and scores perfect.

***Say nothing and leave the docblock's deferral as it was.*** Rejected because the deferral had
already survived several rounds precisely by not carrying a number, and "this wants its own
measurement" is not a decision. The cost of leaving it was demonstrated: a reviewer had to rebuild
the instrument to find out that the obvious fix breaks the build, which is a discovery the next
person would have made the expensive way — by making the change and reading a red ORB as a
regression in their own work.
