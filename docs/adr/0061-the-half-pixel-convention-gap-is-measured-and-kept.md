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
engine's convention shifted by `s`, on the accuracy dataset's own lens: 640x480 at 66 by 50 degrees,
so `fx` 492.757 and **`fy` 514.682**, `cx`/`cy` at 320/240, a 30-degree rotation about `y`.

**The correspondence set is every pixel centre of the frame whose image lands inside the other
frame — 160,000 of 307,200.** A set chosen by a sampling stride is a parameter, and a parameter
nobody records is how this table was wrong the first time. Over **every integer stride from 8 to 32,
starting from the first pixel centre**, the `+0.00` row runs from 0.010301° (stride 29) to 0.011022°
(stride 31) — seven per cent of spread on a figure published to four places, from a choice nobody
would think to write down. Every pixel centre is a property of the lens instead.

| shift | rotation error |
| ----- | -------------- |
| -0.50 | 0.021008° |
| -0.25 | 0.015752° |
| +0.00 | **0.010498°** — what the engine does today |
| +0.25 | 0.005248° |
| +0.50 | **0.000000°** — the correction |

**Asserted by `CameraModelAgainstOpenCV.TheHalfPixelShiftCostsTheAngleADR0061Publishes`**, which
runs this sweep through the engine's own `Unproject` and fails on a row that moves by more than 1e-5
degrees. That test exists because a reviewer rejected this ADR's first argument for not having one, and it
earned its place immediately: sabotaged to use a square lens — which is exactly the mistake the
previous version of this table made — the `+0.00` row fits 0.010395 against the published 0.010498,
ten times the tolerance out.

To be exact about what fails, since a reviewer went and ran it: on the committed test the square
lens is caught one line earlier, by `ASSERT_NEAR(lens.fy, 514.681661, 1e-6)`, so it dies before any
row is fitted. 0.010395 is what the sweep produces with that guard removed. Both are the test doing
its job; only the second is a statement about the table.

Linear in the shift, exactly zero at `+0.5`. Reproduced on the committed `synthetic-ring-4` lens
(48x36, `fx` 36.957, `fy` 38.601, a 7-degree rotation **about `y`**, as above), again over every
pixel centre — 1,488 of
1,728: 0.054141° at `+0.0` and 0.108482° at `-0.5`, twice as a linear term must be, and zero at
`+0.5`. A smaller lens gives a larger angle for the same half pixel, which is what a fixed
image-plane offset should do. Two lenses, one geometry.

The axis is stated because it was not, and it decides the figures: about `x` the same lens and angle
give 1,470 correspondences and 0.027560°, half the published number, since the frame is wider than
it is tall. This row is prose where the row above it is asserted — the test pins the accuracy lens,
which is the one the published table is about, and a second fixture would be a second thing to keep
true for a claim that is already made twice.

**And one column is gone rather than corrected.** There was a "worst reprojection" beside each row,
and it failed to reproduce under both of this table's retractions — the second time against a
reviewer who tried a dozen definitions of it. The ADR never said what the reprojection was *of*. A
figure whose definition cannot be recovered from the document publishing it is not a measurement,
and the rotation column says everything it was there to say.

**This is a smaller number than the raw offset, and the difference is the fit.** The offset is
`(-0.5, -0.5)` pixels, which moves a *bearing* by **0.0805°** at the optical centre and by as little
as 0.0494° toward a corner, where a pixel subtends less angle. A rotation fitted over a whole frame
absorbs most of a constant image-plane translation into itself, so 0.0105° is what survives the fit.
The two are not alternative measurements of one thing — one is what the offset does to a bearing and
the other is what it does to the answer — and the first is 7.7 times the second. Both are asserted:
the second by the test above, the first by
`Unproject.TheHalfPixelOffsetMovesABearingFurtherThanItMovesAFit` — that suite, not
`CameraModelAgainstOpenCV`, because it uses no OpenCV and moved to the file that does not
need it.

`docs/06-roadmap.md` used to quote 0.0581° for that first figure, which is `atan(0.5 / fx)`: one
axis of a two-axis offset, and the axis with the longer focal length at that. It is the smallest of
the three angles in this paragraph and it was standing in for the largest. The same test asserts it,
as the figure it is not.

**And applying it turns the suite red.** Patching that one line and re-running
`EveryDetector/Accuracy.*` against the same rendered ring:

| detector | median today → corrected | max today → corrected |
| -------- | ------------------------ | --------------------- |
| ORB | 0.1009 → **0.2929** | 0.1551 → 0.4713 |
| AKAZE | 0.0612 → 0.0562 | 0.1330 → **0.2566** |
| SIFT | 0.0239 → **0.0626** | 0.0435 → 0.1062 |

ORB fails **both** bounds, not one: `EXPECT_LT(score.medianDeg, 0.2)` on 0.2929, and
`EXPECT_LT(score.maxDeg, 0.4)` on 0.4713. Worth counting, because the reflex after a red suite is
to move the bound and there are two of them to move — and the max bound is the one
`registration_accuracy_test.cpp` documents as deliberately tight, at 2.58 times the worst frame any
detector produces today.

A two-sided sweep of the shift puts SIFT's median minimum at `s = 0` — which is where the model is
still half a pixel out, not where it is exact. The detector measures best where the geometry is
wrong; that is the whole finding, stated as a minimum.

**It is a step rather than a smooth optimum, and "exactly" would oversell it.** SIFT's median is
0.0239° at `s = -0.015625` as well as at `s = 0`, and then jumps 86% to 0.0444° a sixty-fourth of a
pixel the other side. That is a match set or an inlier set flipping, not a bias curve with a
turning point, so the honest claim is that the minimum *contains* zero rather than sits at it. The
conclusion is unaffected: wherever in that flat step the true optimum lies, it is nowhere near
`+0.5`.

Five of the six figures get worse and one does not: AKAZE's median improves by 0.005 while its worst
frame nearly doubles. That one is the reason to read the max column as well as the median, and the
reason this ADR does not say "every figure gets worse".

**So the published table is green partly by cancellation.** This model error runs against whatever
sub-pixel localisation bias the three detectors carry, and removing one of the two leaves the other.

**The evidence for that is the sweep, not a log line.** Each detector's median is minimised at a
*different* shift — SIFT at about 0, ORB at about `+0.25` (0.0918°, better than either end), AKAZE
at or beyond `+0.5`. A single shared cause, the renderer's 3.4x bilinear upsample being the obvious
candidate, would put all three optima in one place; three optima in three places is a per-detector
localisation bias, measured in this repository's own units. OpenCV also says something adjacent in
passing while the suite runs — *"SIFT_Impl precise upscale disabled, this is now deprecated as it
was found to induce a location bias"* — but it is `CV_LOG_ONCE_INFO` from `SIFT_Impl`'s constructor
and says nothing about ORB or AKAZE, so it is corroboration for one detector rather than the
argument for three.

The chaining-and-gauge-removal absorbs a further part of a per-step conjugation, which is why the
per-pair 0.0105° and the per-chain medians are not the same quantity and cannot be subtracted from
one another.

## Decision

**Keep the convention gap for now. Publish its size and its direction. Name what a correction has to
move, so the next person starts from a measurement rather than a sign.**

Concretely:

1. `ReadBearings` is unchanged. The line stays
   `const Pixel pixel{static_cast<double>(xy[0]), static_cast<double>(xy[1])};` — quoted exactly,
   because an earlier version of this ADR paraphrased it as `Pixel{xy[0], xy[1]}` and a
   paraphrase of the one line a document turns on is not a citation of it.
2. The `Bearing` docblock carries the red-suite table — not the shift table, which lives here —
   and the sentence that matters operationally:
   **a red ORB after the correction is not an estimator regression.** It is the engine becoming
   honest about its geometry while the bound above it still encodes the cancelled number.
3. `docs/06-roadmap.md` keeps its live table and its existing paragraph saying the bearings are half
   a pixel out, and now points here for what correcting them costs.
4. When the correction is made it moves **in one commit**, and the list of what moves is
   **already written**: the comment headed **"Where else the table is written"** in
   `registration_accuracy_test.cpp` catalogues every figure site, the `+ 0.5` in `ReadBearings`,
   and the rule that ADRs 0059 and 0061 are *superseded* rather than corrected. Read that
   catalogue, not a list here.

   The two `EXPECT_LT` bounds are deliberately **not** entries in it — they are what *fails* when
   the figures move, rather than prose that has to be corrected. Its first sentence names both, for
   the reason a reviewer gave: a correction that moves one and forgets the other leaves a red suite
   for a cause the commit message will not mention. An earlier version of this paragraph claimed
   they were in the list, when only the max bound appeared, in passing, as the assertion one figure
   sits under.

   **Named by its heading rather than by a line range, which is the third version of this pointer.**
   A five-item list came first and reached two of the eight it was duplicating. Then `516-541`,
   which stopped before two of the three things it was cited for. Then `516-558`, which went stale
   the moment the catalogue was edited to hold the entries this change adds. A range into a comment
   that grows is a copy of a fact in disguise — it is the line numbers that drift instead of the
   figures — so this points at a string a reader can search for.

   This ADR added two entries to that catalogue. The `+ 0.5` in `ReadBearings`, which is not a
   figure and is listed apart from the ten for that reason. And the `Bearing` docblock, which **is**
   one: it spells the live table's today column in a source comment the catalogue had never listed,
   and this change writes another figure into it. §2 says the shift table lives here and not there,
   which is true of the shift table and was never true of the detector table beside it.

**Why not simply correct it now.** Because the honest correction is not one line, and the one-line
version is worse than the gap. Moving `ReadBearings` alone leaves a red build. Moving the bound with
it publishes a *larger* error as the project's accuracy while claiming an improvement — the
improvement is in the model, the number is in the measurement, and they move opposite ways. A reader
now has this ADR to explain that much, which is why the first rejected alternative below is rejected
on one ground rather than two; what is still missing is not the explanation but the separation.

What makes the correction defensible is a measurement that separates the model error from the
detector bias, and this repository cannot take it yet: it needs a dataset
whose keypoint localisation is known rather than inferred, which is a harness increment of its own
alongside the noise, blur, rolling-shutter skew, exposure ramps, bursts and movers ADR 0050 names.
(Distortion is *not* on ADR 0050's list — `docs/06-roadmap.md` says so and says it belongs there —
so it is an increment this repository owes rather than one it has already written down.)

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
found to have been measured wrongly, they are retracted **in a new ADR** rather than quietly
replaced — which is what 0057 means by retraction and is not the same operation as the corrections
below.

**The shift table was wrong twice before this ADR merged, and both times it was corrected rather
than retracted, because nothing had been published yet.** The first version's six decimals came from
a probe whose correspondence set was never recorded. The second was measured on a lens with square
pixels, which the accuracy dataset's is not — `LensFromFieldOfView` solves the two axes from two
different angles, so `fy` is 514.68 where `fx` is 492.76. An unmerged ADR is a draft and a draft is
edited; the retraction rule starts the day it lands. Both mistakes are recorded here rather than
erased, because two independent rebuilds of one described rig disagreeing is the argument for the
tests above.

**ADR 0060's precedent applies here, and this ADR argued that it did not.** The argument was that
0060's rule — assert a published figure from a test rather than leaving it as prose — is about
figures in *source comments*, and that an ADR is never edited into agreement with the present, so a
test pinning one would be asserting history and would go red as a way of announcing that an ADR had
aged.

That is wrong, and a reviewer said why: it is equally true of every figure 0060 did pin, and going
red *is* the mechanism. A failing assertion is not a demand to edit an ADR; it is the trigger to
supersede one, which is the operation `docs/adr/README.md` already names. The substitute this
section offered — that the rig is reproducible from the paragraph alone — is exactly what failed,
twice: two readers reconstructed the described rig and got two different tables, because the
description omitted that the lens is not square.

So the table is asserted now, by
`CameraModelAgainstOpenCV.TheHalfPixelShiftCostsTheAngleADR0061Publishes`, and the per-bearing
figure beside it by `Unproject.TheHalfPixelOffsetMovesABearingFurtherThanItMovesAFit`. What the tests pin is
the geometry, not the accuracy table: they need no dataset and no detector, so an OpenCV bump cannot
move them and a red row means the model changed. When one does go red, this ADR is superseded rather
than corrected.

**The bounds stay at 0.2 and 0.4, and the phase threshold at 0.5 degrees.** All three were written
against the numbers the engine actually produces. Correcting the geometry without re-deriving them
would leave three constants tuned to a world that no longer exists, which is the failure ADR 0057
records.

## Rejected alternatives

***Correct `ReadBearings` and relax the bound to match.*** One line plus one constant, and the build
is green again. **The obvious objection to it no longer holds, and saying so is the point of
writing this down:** "nothing in the tree would explain why a correctness fix published a worse
figure" was true until this ADR existed, and this ADR is that explanation. What remains is the
objection that survives: it spends the bound — the one gate standing between this engine and a
silent regression — to pay for a change that was supposed to improve it, and it spends *two* of
them, since the max bound fails as well. A bound that moves whenever it is inconvenient is not a
bound. So this alternative is now rejected for one reason rather than two, and on the day the
separating measurement named above exists it becomes the right answer rather than a rejected one.

***Correct `ReadBearings` and leave the bound, accepting a red build until the estimator improves.***
Honest, and unusable. A red `main` is a signal nobody can read: the next real regression arrives into
a suite that is already failing, and the failure that matters is invisible among the one that is
expected. This project's whole argument for a measured bound is that it distinguishes a regression
from a re-tuning, and a permanently red test cannot.

***Split the difference — apply a half-pixel correction to the renderer instead, so the two
conventions meet in the middle.*** Rejected because it makes the *dataset* wrong in order to make
the *engine* look right. The renderer's convention matches `camera_model`'s and is correct; moving it
would mean every dataset this repository produces encodes a compensating error. That is a cousin of
the shared-error failure ADR 0050 exists to prevent rather than the same one — 0050's case is a
dataset rendered *through the code under test*, where a shifted renderer would be a second
self-consistent convention that disagrees with any real OpenCV calibration — but the symptom is
identical: it renders wrong, registers wrong in the compensating way, and scores perfect.

***Say nothing and leave the docblock's deferral as it was.*** Rejected because the deferral had
already survived several rounds precisely by not carrying a number, and "this wants its own
measurement" is not a decision. The cost of leaving it was demonstrated: a reviewer had to rebuild
the instrument to find out that the obvious fix breaks the build, which is a discovery the next
person would have made the expensive way — by making the change and reading a red ORB as a
regression in their own work.
