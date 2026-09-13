# 0060 — The refused-row reachability figure was wrong, and is pinned by a test now

**Status:** accepted

## Context

`feature_registration_engine.cpp`'s `Bearing` docblock argues that `ReadBearings` keeping a refused
row in place — rather than skipping it, which once shifted every later correspondence and turned a
perfect registration into 8.2257 degrees — is a correctness fix and not a hypothetical. The argument
is that `Unproject` genuinely refuses keypoints on lenses people own, and it was made with two
measured figures:

> a 78-degree lens with `k1 = -0.20` refuses about 4% of in-frame pixels, and a 100-degree one with
> `k1 = -0.35` refuses 38.8%.

The first is right. The second is wrong by a factor of about one and a half in the direction that
understates the case: such a lens refuses **66.8%** of its own frame, measured at every pixel centre
of a 320x240 frame, and the figure is insensitive to frame size — over 80x60, 160x120, 320x240,
640x480 and 1280x960 it spans 66.7474% to 66.9167%. There is no vertical field of view that makes it
38.8% either: holding the horizontal angle at 100 degrees and `k1` at -0.35, the refused share runs
from 45.6% at a 10-degree vertical angle to 83.9% at 120, and never passes through 38.8%. Measured
with the core's own `Unproject`, and separately with the dataset renderer's independent
re-implementation of the same lens, which agrees to four decimal places.

Nothing could have caught it. It was prose beside code that cannot fail on it — the same shape as
every figure ADR 0057 was written about, one layer down, in a source comment rather than a document.

## Decision

Retract 38.8%; publish 66.8%; and **assert both figures from a test** rather than leaving them as
prose, so the paragraph cannot drift a third time.
`Unproject.TheTwoLensesTheEngineCitesRefuseTheFractionsItCites` measures each lens over its own
frame and asserts the share to within half a percentage point — three times the spread across the
frame sizes above (0.169 points for the ultra-wide lens, 0.146 for the wide) and a fifty-sixth of
the 28-point error it caught. Those two multiples read "four times" and "a tenth" in the first
draft of this ADR, in both tests and in the commit message. A reviewer divided. That is the second
uncomputed figure this one change produced, which is the argument for the decision above rather
than an embarrassment beside it: prose next to code that cannot fail on it drifts at a rate that
does not care what the prose is about.

A second test records the thing the first one makes obvious once the numbers are in front of you,
and which was not written down anywhere: **the refused-row path is unreachable from any rendered
dataset**. `tools/synth_dataset.py` refuses to render a frame with a rayless pixel in it — there
being no colour that could honestly stand for a missing direction — so the lens of every dataset
this repository produces answers every pixel *centre* of its own frame. Adding distortion to the
renderer, which `docs/06-roadmap.md` lists as an increment and gives this guard as a reason for,
will not reach it.

The retraction lives here and the docblock keeps the live figure and a pointer, per ADR 0057.

**Centres, not keypoints.** That last claim is about pixel centres, and a keypoint is not one:
`ReadBearings` passes OpenCV coordinates through unchanged, and OpenCV puts a pixel's centre at an
integer where this model puts the image's corner at the origin, so a keypoint reported at (0, 0)
arrives half a pixel further out than any centre. Bisected on a 66x50 degree lens, every centre
survives down to `k1 = -0.23340` and the corner only to `-0.23178` — a narrow band of lenses the
renderer would accept and the core would refuse a corner keypoint on. Nothing is in that band today
and no dataset is distorted at all, so this changes no conclusion; it is recorded because the first
draft said "by construction", which is a stronger word than the measurement supports.

## Consequences

- The two figures are now derived where they are checked and copied where they are read, with a
  test between them. The copy in the docblock can still be edited into disagreement with the test,
  which no tooling prevents; what it cannot do is disagree with reality while the suite is green.
- The test walks 76,800 pixels twice through a damped-Newton inverse. Measured at 307 ms in the
  debug build, which is why the frame is 320x240 rather than a phone's: the answer does not depend
  on the resolution and the cost does.
- **The roadmap's reason for rendering with distortion is now known to be partly wrong**, and that
  matters more than the figure. Distortion in the renderer is still worth having — it is the
  difference between measuring registration on a lens a phone has and one nobody sells — but it
  will not exercise the refused-row path, and a test written to drive that path by rendering a
  distorted dataset would pass without ever taking it. Reaching it needs a lens whose fold is
  inside the frame, which is exactly the lens the renderer will not render; that is its own
  increment and is not this one.
- ADR 0057's rule is now applied to a figure published in a source comment rather than in `docs/`.
  That was already the reading — its own Rejected section notes the carrier need not be an ADR —
  but this is the first time it has been exercised, and the trigger list in
  `docs/00-principles.md` needs no change for it.

## Rejected alternative

**Correct the number in place and say nothing.** This is what the trigger exists to prevent, and it
is more tempting here than it was for ADR 0057's figures, because a percentage in a source comment
feels like colour rather than a publication. It is not: it is the whole of the argument for why
`Bearing` carries a flag instead of a `continue`, and someone weighing whether that flag is worth
its complexity would be weighing it against a number that is wrong by 28 percentage points in the
direction that makes the flag look less necessary. A figure that is load-bearing for a design
decision is published whatever file it sits in.

**Delete the figures and argue qualitatively** — "some lenses refuse some pixels". Cheaper, and it
would have been immune to this defect. It also throws away the only thing that makes the paragraph
convincing: "a wide-angle phone lens refuses 4.5% of its frame" is an argument, and "distortion can
make pixels unprojectable" is a restatement of `Unproject`'s signature. The measurement is the
point, so the answer is to measure it rather than to stop saying it.
