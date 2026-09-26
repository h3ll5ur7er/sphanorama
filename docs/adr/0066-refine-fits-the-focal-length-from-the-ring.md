# 0066 — `Refine` fits the focal length, from the inlier matches each pair now carries

**Status:** accepted

## Context

ADR 0065 wired `Refine` to the rotation solver and passed the lens through, because refining it
needs matched points and a `PairwiseResult` carries only how many there were. On a phone the lens
handed in is the page's reported field of view, which is a guess: nothing on the web platform
reports a camera's focal length, and the field of view a page can infer is the preview's, not the
sensor's. So the question this ADR answers is first how much a wrong focal length costs, and it was
measured before anything was designed.

**The measurement.** The photograph ring of ADR 0059, twelve frames at 640×480, priors three degrees
out, closing pair included — the setup of the solved-ring row in the roadmap's table — with the focal
length handed to both `EstimatePairwise` and `Refine` scaled away from the truth:

| focal scale | ring closure | ORB median | AKAZE median | SIFT median | solved edge median (ORB/AKAZE/SIFT) | pair residual px (ORB/AKAZE/SIFT) |
| --- | --- | --- | --- | --- | --- | --- |
| 0.90 | 34.1° | 2.54° | 1.77° | 1.24° | 1.47 / 2.09 / 2.46° | 1.38 / 1.30 / 1.55 |
| 0.95 | 16.2° | 1.25° | 0.89° | 0.45° | 0.66 / 0.97 / 1.22° | 1.04 / 0.74 / 0.98 |
| 0.98 | 6.3° | 0.50° | 0.36° | 0.16° | 0.25 / 0.38 / 0.48° | 0.81 / 0.56 / 0.47 |
| **1.00** | **0.2°** | **0.055°** | **0.035°** | **0.026°** | **0.010 / 0.011 / 0.013°** | **0.80 / 0.47 / 0.26** |
| 1.02 | 6.4° | 0.45° | 0.36° | 0.19° | 0.26 / 0.36 / 0.45° | 0.77 / 0.54 / 0.46 |
| 1.05 | 15.3° | 1.10° | 0.80° | 0.40° | 0.62 / 0.89 / 1.12° | 0.90 / 0.71 / 0.90 |
| 1.10 | 29.2° | 2.12° | 1.48° | 0.72° | 1.26 / 1.71 / 2.12° | 1.15 / 1.06 / 1.52 |

"Ring closure" is the angle of the product of the twelve pairwise rotations, zero for a ring that
closes. Three things follow.

- **Two percent is the whole budget.** ORB's median crosses Phase 2's 0.5-degree exit threshold at a
  focal length 2% out, and every detector is past it at 5%. A field of view read from a browser is
  not known to 2%, so the exit criterion cannot be met on a phone with the lens passed through.
- **A pair cannot see the error.** Its own pixel residual at 2% out is within noise of its residual
  at the truth — 0.81 against 0.80 pixels for ORB, and 0.77 on the other side. A rotation about the
  viewing axis's perpendicular is almost exactly a translation of the image, and a translation fits
  under any focal length; what the wrong focal length changes is *how many degrees* that translation
  is taken to be. So no per-pair fit, and no amount of care in `EstimatePairwise`, can estimate it.
- **A loop sees it at once.** The ring fails to close by about 3.2 degrees per percent, and
  `Refine`'s own solved edge median rises twenty-five-fold at 2%. The frames agree with each other
  only under the true focal length. This is the classical observation that a full turn has to add up
  to 360 degrees, and `Refine` already computes the quantity that measures it.

## Decision

1. **`PairwiseResult` carries its inlier matches** as `std::vector<PixelMatch>`, each the pixel in
   frame `a` and the pixel in frame `b` of one correspondence the returned rotation agrees with to
   within the engine's inlier gate — the rows `inliers` counts. Not the rows it was fitted on,
   which are the inliers before the last re-gate (see Consequences). Pixels, not bearings: a bearing is a pixel already pushed through a lens, so bearings would
   freeze the very guess this ADR exists to correct. Filled on every `Ok` result, accepted or not,
   and bounded by the feature cap (`kMaxFeaturesPerFrame`, 500) since each row of frame `a`'s
   feature set matches at most once. A default `PixelMatch` is NaN rather than zero, so a match
   nobody wrote is refused rather than read as the image's corner.

2. **`Refine` fits one focal scale shared by `fx` and `fy`**, by a bracketed one-dimensional search
   over the scale that minimises the solved edge error. At each candidate scale every accepted
   pair's rotation is refitted in closed form (Kabsch, which the engine already has) from its inlier
   matches unprojected through the candidate lens, and the rotations are solved as ADR 0065 solves
   them. No RANSAC is re-run: the inlier sets were chosen under the initial lens, which the table
   shows is close enough that every pair was accepted at 10% out. The aspect ratio, the principal
   point and the distortion coefficients are kept from `initial` — the aspect because the pixel
   grid fixes it, the rest because a rotation-only ring does not constrain them well and nothing
   has measured whether it does (see Rejected alternatives).

3. **The fit is only taken where it is an answer.** A tree of accepted pairs agrees with itself
   under any focal length, so the error is flat and the minimum is wherever the search stopped.
   `Refine` searches only over the frames the solve places — an edge in a component nothing anchors
   is left out of the solve, and was once scored against the identity it gives those frames, which
   pulled the fit by nearly 1% — and only where their accepted pairs close a loop, two pairs between
   the same frames counted once. Every scale is scored on the same matches, the ones with a direction
   at the bracket's shortest focal length: a scale that lost a pair's matches to a folding lens
   once scored as infinite, and infinity passed for a cost that rose. A pair left with fewer than
   three is not refitted and the lens is passed through. And if any trial of the search still could
   not be scored, there is no fit: the first version of this rule assumed a longer focal length only
   brings a pixel nearer the centre, which is true of a radial lens and not of a tangential one —
   `Unproject`'s accepted pixels are not a star about the centre, and one match of ninety that lost
   its direction partway along put a fit 3.2% out, the golden section settling at the edge of the
   window it could not score (a reviewer's reproduction, round 2).

   **And it takes the result only where the least is precise.** Where any of these fails — no
   loop among placed frames, a pair left with fewer than three matches, a trial not scored, a cost
   that does not rise on both sides of the least, a least not precise — `initial` comes back as
   given, every field of it. The rise refuses a least at an end of the bracket, where the cost past
   it falls. The bracket is 0.7 to 1.4 times the focal length handed in, which is chosen rather than
   measured; what was measured is that every pair of the photograph ring is still accepted at 10%
   out.

   Precise means two errors, combined as independent ones, within two tenths of a percent of focal
   length — about a twentieth of a degree of ORB's median: how far the pairs' noise could have moved
   the least, and how far a lens model a thousandth of the focal length wrong at the frame's corner
   does. Independent, because the second is a scale for a lens error of unknown size and sign, not a
   bias of known size, and is read as one standard deviation of it. Summing them instead would also
   refuse the uneven ring at 1.5 px, 0.17% and 0.09% (round 8).

   The first is estimated from what the least cannot move: each pair's own residual after its
   Kabsch fit (two coordinates a match, three spent on the rotation), never taken below the pairs'
   pooled one, carried through each pair's information about its own rotation — a pair measures
   rotation about its viewing axis far less well than across it — and weighed against how each
   edge's error moves with the scale, read from two trials half a percent either side of the least.
   A pair measured twice counts once, its two copies sharing one noise. That gives the least's
   standard deviation in log focal scale. Over seeds 1 to 200 of noise at 0.4 to 2 px, the least's
   error in these deviations has a root mean square of 0.83 to 0.95 on every shape fitted — a
   twelve-frame ring, a two-by-four grid, a ring of uneven pairs — and reached 2.8 at the most; a
   reviewer's seeds 9000 to 9099 put the ring's at 1.15, one seed at 4.1, so the estimate is good to
   about 15% either way (round 7). It
   barely moves between seeds, which is what lets a threshold decide: the ring 0.007 to 0.037%, the
   grid 0.07 to 0.38%, a triangle or a ring whose one loop skips a frame 0.39 to 2.8%, and the
   photograph ring's pairs — every detector, up to 1.5 px of noise added — 0.005 to 0.14%.

   The second is measured by misreading the lens: its k1 moved until the frame's furthest corner
   lands a thousandth of the focal length out, every pair refitted under that at the least — one
   solve more — and the
   change in each edge's error projected on how the errors move with the scale. It is not noise. An
   unreported distortion is the same on every match and every loop, so no number of either averages
   it away, and a weak loop reads it through the same curve of the tangent it reads the focal length
   through. The ring and the grid read 0.046% and 0.044%, and a real distortion that size moves
   their least by that within 4%, at 640 x 480 or twice that, and on a lens handed in with barrel
   distortion; the photograph ring 0.034 to 0.047%; a triangle, a ring whose loop
   skips a frame, and a ring open at one pair with a chord across every other frame 0.27 to 0.33%.
   The ring's least moves and its rotations do not: it absorbs a k1 of -0.01 — 2.6 px at the corner
   — into a focal length 0.24% short, and -0.03 into 0.72%, with its rotations within a hundredth of
   a degree, because its loops make the lens it fits consistent. Radial only, and a k1 only; see
   Consequences.

   **A fraction of the focal length, not a count of pixels.** A thousandth of it is half a pixel on
   the 640 x 480 frames every test uses, and that is how it was first written: half a pixel. But a
   lens's distortion does not change with the size of the frame it is read into, and a misreading
   counted in pixels halves at the 1280 x 960 the page grabs at. There the chords shape read 0.15%
   and was fitted 1.6% out under a k1 of -0.01, round 7's failure back (round 8). The thousandth is
   chosen, not measured: what a phone's ISP leaves is not measured here. It is read at the corner's
   undistorted radius, the one the added k1 acts on; read at the pixel's, it was a quarter too small
   on a barrel lens and a quarter too large on a pincushion one (round 8). A lens that gives its own
   corner no direction cannot be misread there, and is not fitted.

   **Not however far the least lies from the lens handed in.** A least many of its own spreads from
   the lens handed in looks like evidence that lens is wrong, and a weak loop under a lens model a
   little wrong produces exactly that. So a weak loop leaves the lens handed in alone, right or 8%
   out, and correcting a lens it cannot see precisely is for a capture with the loops to do it, or
   for a lens a device keeps.

   **The loop check is not implied by the rest.** The precision refuses an open eleven-pair chain
   today, at 0.65 to 0.71% on exact matches, but only because its pairs' noise happens to exceed
   the threshold. A chain's least is where its priors — three degrees out, at a hundredth of an
   inlier each — pull the cost to a minimum of their own, and neither the pairs' noise nor the lens
   model measures that pull; under an earlier rule, with the check removed, the chain was fitted to
   502.7 of 500. The check refuses a chain for what it is, and leaves its spread infinite.

   **How the rule got here.** Each version before this one was found wanting by a reviewer's Monte
   Carlo, and what each got wrong is the reason for a part of what replaced it.

   - *Round 3.* The cost at both ends of the bracket four times the least. A skipping ring at
     0.8 px of noise — ORB's pair residual — passed it in forty seeds of forty, up to 1.5% out, with
     rotations ten times worse than the right lens gave and an edge error that read clean.
   - *Round 4.* The cost doubling within half a percent of the least. It still took that ring in six
     seeds of two hundred, 2.2% out, while refusing the photograph ring's ORB fit in four seeds of
     ten once 1.5 px of noise was added, sending back a lens 5% long with the least within 0.25%.
     Both rules judged the least by the cost at it — how far the loops fail to close there — and
     that is a chi-square on the few degrees of freedom the loops leave, independent of the error
     that moved the least: it varied 140-fold across ten seeds while the least moved a quarter of a
     percent. Judging by it selects nothing.
   - *Round 5.* The precision, with the pairs' residuals pooled outright, which is exact only where
     every edge's error moves alike with the scale. The solve puts a loop's misfit where the pairs
     say least, so a thin pair's noise counts for most where a pool dilutes it: read from the pool, a
     ring with one pair of fifteen matches at twenty times the others' noise is fitted in every seed,
     up to 0.8% out, and read per pair it is refused in every one (round 7's reproduction). It also
     counted a pair measured both ways twice, reading the spread √2 tight.
   - *Round 6.* The precision, plus a clause taking an imprecise least four or more of its spreads
     from the lens handed in as refuting that lens. Under the unreported k1 of -0.01 the skipping
     ring and the triangle put their least 1.6% out on exact matches, ten of their spreads away,
     and were fitted there, the rotations seven times worse; at -0.03, 5.0 to 5.1% out. The clause
     went, and a floor went in: no match read as agreeing to better than half a pixel.
   - *Round 7.* The floor was on each match's noise, so it shrank as a shape's matches and loops
     grew, and a lens model's error does not. The ring open at one pair with ten chords read 0.15 to
     0.17% through it at 0.4 px and was fitted 1.8% out under the same k1, its rotations six times
     worse. The floor went, and the misreading replaced it.
   - *Round 8.* The misreading was half a pixel, which halved at the resolution the page grabs at,
     and read the corner at its distorted radius. It is a thousandth of the focal length now, at the
     undistorted radius.

   **A loop need not wrap the sphere.** The first version of this decision said a triangle turning
   about one axis closes under any focal length, since three angles that sum to zero still do when
   scaled, and its test asserted the triangle was passed through. It came back fitted, to 499.9967
   of 500, on exact matches: under a pinhole a turn moves a pixel by the tangent of its angle, not
   the angle, so under the wrong focal length a thirty-degree pair and a sixty-degree one are not
   scaled alike and the triangle stops closing. The first version of this paragraph said the pairs
   tilted; a reviewer's independent rebuild showed their axes stay exactly vertical. Its least is
   right, but a lone triangle is precise only to 0.39% or worse at 0.4 px of noise, and a lens
   misread by half a pixel moves it 0.3%, so it is not taken — seeing the focal length and seeing it
   precisely are different questions, and the second is the one a capture asks. A grid of such
   loops, none wrapping, reads 0.07% and is.

4. **`GlobalSolution::lensFitted` says which happened.** Not `Intrinsics::estimated`, which the
   first version of this decision used: that field says whether a lens was ever estimated, and a lens
   a device kept from an earlier capture is an estimate this call did not make. So a lens passed
   through keeps `estimated` as it was given, and a fitted one has it set.

   **And `GlobalSolution::focalSpread` and `focalModelError` say how sure.** The least's standard
   deviation in log focal scale, and how far the half-pixel misreading moves it, each reported
   wherever the search reached a least with the cost rising on both sides, taken or not, and infinite
   where it did not — an end of the bracket is no least — or where a trial could not be scored.
   Reported apart, because they combine differently across captures: another capture of the same
   lens averages the noise down and leaves a model error where it was.
   A lens kept across captures will need it to weigh one capture's fit against another's, and it is
   what a test can hold the estimate to: until it was reported, the calibration above was prose, and
   a reviewer found every part of the estimate — the degrees of freedom, the frame a pair's
   information is gathered in, the frame of its error, the weights — could move it 10 to 20% with
   the whole suite green, since every decision the tests asserted sat far from a threshold (round 5).

5. **Measured before it is bounded.** The accuracy harness gains the solved ring handed a lens 5%
   and 10% out on both sides, and asserts the recovered focal length and the median rotation error
   against numbers taken from the first implementation, in the manner of ADR 0057.

## Consequences

- **Measured, on the photograph ring** with the focal length handed in 10% and 5% out either way:
  recovered to within -0.063 to -0.051% (ORB), -0.004 to +0.002% (AKAZE) and +0.010 to +0.012%
  (SIFT), and the ring solved to medians of 0.047–0.052, 0.029–0.034 and 0.010–0.019 degrees — as
  well as it solves handed the right lens. `AFocalLengthOutIsFittedFromTheRing` bounds it per
  detector at about one and a half to two times each measurement — the focal length within 0.1%,
  0.01% and 0.02%, and the median, worst frame and edge median likewise.
- **The solved-ring figures move**, lens right or not, from 0.0552 / 0.0348 / 0.0264 to 0.0464 /
  0.0293 / 0.0142 degrees, and that answers ADR 0065's open question of why SIFT's solve sat a little
  above its chain. The answer is in the pairs. `FitRotation` refits its rotation on the RANSAC
  inliers, re-gates, and reports the inliers after the re-gate beside the rotation fitted before it,
  so an engine's pair is not the least-squares rotation of the matches it reports. Refitting each
  pair on those matches — which the search does at every scale — gives 0.0375 / 0.0291 / 0.0147 at the
  rendered focal length with no search at all. **Not uniformly better, though**: on the open
  eleven-pair chain the refit reads 0.1353 / 0.0344 / 0.0229 against the pairs' own 0.1005 /
  0.0601 / 0.0229, so ORB's chain is worse refitted. Which is why `Refine` uses the refit only when
  it fits the lens, where it has to — the refitted pairs are what the fitted lens was scored on —
  and the pair's own rotation otherwise, rather than preferring one copy everywhere on a measurement
  that points both ways. Making `FitRotation` return the rotation of the inliers it reports removes
  the disagreement at its source; that is its own change, since it moves the chained table.
- A contract change in `types.h` (`PixelMatch`, `PairwiseResult::inlierMatches`,
  `GlobalSolution::lensFitted`, `GlobalSolution::focalSpread`, `GlobalSolution::focalModelError`)
  and in `engines/registration_engine.h` (`Refine`'s paragraph on the lens, and a refusal for
  matches that disagree with the inlier count). The TypeScript mirror moves with them: the generator
  mirrors every type in `types.h`, though no registration type crosses at runtime.
- **Memory.** Sixteen bytes a match as four floats, at most 500 a pair: 8 KB a pair, and a sphere of
  sixty frames registered against four neighbours each is about 2 MB of matches held until `Refine`
  returns. Small against the frames, but a new allocation per pair and now part of what a build
  holds.
- **Time.** `Refine` becomes twenty-five solves instead of one — the solve at the lens handed in,
  two to open the golden-section search, nineteen to narrow it to 1e-4 in log scale, one half a
  percent either side of the best, which is where the precision is read, and one at the best under
  the misread lens. Each is cheap next to feature
  extraction, and how cheap on a phone is a measurement the WASM build will have to take once
  registration compiles there (ADR 0047).
- **A capture without strong loops keeps its guess**, and says so through `lensFitted` — even a
  guess 8% out, four times what the Context's table says the exit criterion allows. An open strip
  closes no loop. A triangle, a ring whose loop skips a frame, or a ring open at one pair with a
  chord across every other frame closes loops too weak to see the focal length past a lens model a
  thousandth of the focal length out. A ring open at one pair with two chords, handed a lens 5% out with 0.2 px of
  noise, keeps it at a median of about 4 degrees where a fit would have reached 0.08 (a reviewer's
  probe, round 7). And a two-by-four grid, fitted at 0.8 px of noise, is refused at 1.25 px and
  above. Only twelve-frame rings and a two-by-four grid have been shown to fit; that a sphere's loops
  are strong enough is expected, not measured. The rest is what the lens a device keeps is for.
- **A k1, and radial only.** The misreading is one shape of radial distortion. Others of the same
  size move the least by other amounts: a k2 or a k3 that moves the corner as far, a half to a
  quarter as much; a mustache that peaks at that size inside the frame and is zero at the corner,
  2.5 to 2.9 times as much — 0.131% on the ring against 0.046% reported, its rotations unmoved (a
  reviewer's probe, round 8). So `focalModelError` is a scale that separates weak loops from strong
  ones, six to one, and not a bound on every lens error; a lens kept across captures has to read it
  that way. And a lens model can be wrong otherwise than radially.
  Under a rolling shutter with 3 to 10 degrees a second of hand shake, or an unreported tangential
  distortion, a reviewer's grids handed the right lens were fitted 0.25 to 0.5% off, their rotations
  1.5 to 1.7 times worse and the spread understating the error two- to five-fold — though that is at
  most 0.035 degrees of median at 5 degrees a second (round 7). A principal point 10 to 40 px off
  centre costs the rotations whether the lens is fitted or not. `Intrinsics` carries a rolling
  shutter's line time for the day it is modelled.
- **Only the focal length.** A phone's image is expected to arrive with most of its distortion
  corrected by the ISP — expected, not measured here — which would leave the focal length as the
  error that matters; a lens whose residual distortion matters needs the bundle adjustment below.
- **`Refine` now needs a Kabsch**, which in this engine is OpenCV's SVD. The rotation solve under
  it still needs none, but a `Refine` built outside this engine — the browser route ADR 0062 named
  for `rotation_averaging` — has to bring its own, from the core's Jacobi eigensolver or otherwise.
- **The lens a device keeps between captures** is the next decision and is left to its own ADR: it
  is a new volatility axis — what this device's camera is known to be — with no owner in the
  volatility map, and the natural `initial` for this search.

## Rejected alternatives

**Full bundle adjustment** — Levenberg–Marquardt over every rotation and the lens together, on the
reprojection error of every match. It is the general answer and it would fit distortion as well.
Rejected *for this step* because the measurement says the focal length is the error that breaks the
exit criterion, a one-parameter search over the solve that already exists is the smallest change
that reaches it, and a bundle adjustment brings a nonlinear solver this core does not have, along with
the question of whether a rotation-only ring constrains distortion at all. It stays the route for
distortion, and the carried matches are the input it would need.

**Estimate the focal length per pair.** The table's last column is the argument against: the pair
residual does not move at 2%, and is not even minimised at the truth for ORB.

**Close the ring by formula** — scale the focal length so the yaw angles of a horizontal ring sum to
360 degrees. Exact for one ring about one axis, and a sphere is several rings, tilted, with
cross-ring pairs. The solved edge error is the same quantity generalised to any graph with a loop.

**Carry bearings instead of pixels.** Cheaper to consume, and fixed to the lens they were unprojected
through, which is the lens being corrected.

**A floor on each match's noise.** Round 6's answer to a lens model a little wrong: no match read
as agreeing to better than half a pixel. It was the right size and the wrong shape. A floor per
match is a noise, and a noise shrinks as the square root of the matches and loops that average it,
while a lens model's error is the same on all of them — so a shape with enough matches and loops
read under the threshold through the floor, and was fitted 1.8% out (round 7). Measuring what the
misreading does to the least costs one solve and does not shrink.

**A width at the least: the cost must double within half a percent of it.** This decision's
second rule, and the one this paragraph once rejected the standard error in favour of, because a
count of independent loops seemed to be what a standard error needed and a ring's shared frames do
not give one. The count was never needed: the pairs' own residuals give the noise directly, and a
loop cannot absorb them. The width was judged against the cost at the least, which is the loops'
leftover disagreement — a statistic on two or three degrees of freedom, not the noise — and a
reviewer's probes showed it taking the skipping ring 2.2% out in six seeds of two hundred, and
refusing the photograph ring's ORB fit, within 0.25%, in four seeds of ten (round 4).
