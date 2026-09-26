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

   **And it takes the result only where the least is precise.** The
   precision is estimated from what the least cannot move: each pair's own residual after its
   Kabsch fit (two coordinates a match, three spent on the rotation), never taken below the pairs'
   pooled one, carried through each pair's information about its own rotation — a pair measures
   rotation about its viewing axis far less well than across it — and weighed against how each
   edge's error moves with the scale, read from two trials half a percent either side of the least.
   A pair measured twice counts once, its two copies sharing one noise, and no match is read as
   agreeing to better than half a pixel, since the lens model it is read through is not good to
   better. That gives the least's standard deviation in log focal scale. The fit is taken when it
   is within two tenths of a percent, about a twentieth of a degree of ORB's median, and not
   otherwise. The cost must also rise on both sides of the least, which refuses a least at an end of
   the bracket, where the cost past it falls.

   **Not however far the least lies from the lens handed in.** This rule once also took an
   imprecise least that lay four of its spreads or more from the lens handed in, as refuting the lens
   a refusal would fall back to. But the spread is the pairs' noise and nothing else, and a lens model
   a little wrong moves a weak loop's least by more: such a loop reads the focal length through the
   curve of the tangent and reads distortion through the same curve. Handed the right focal length
   with an unreported k1 of -0.01, 2.6 px at the corner, the skipping ring and the triangle put their
   least ten of their own spreads away, 1.6 to 2.5% out, and were fitted there, the rotations seven
   times worse; at -0.03, 5.8 to 7.1% out (a reviewer's probe, round 6). The same distortion with
   exact matches read the skipping ring's own residuals as precise to 0.16% and fitted it 1.6% out,
   which is why the half-pixel floor exists: with it the ring reads 0.45%. A precise shape moves
   too — the twelve-frame ring absorbs k1 of -0.01 into a focal length 0.24% short, -0.03 into 0.72%
   — but keeps its rotations within a hundredth of a degree, because its loops make the lens it fits
   consistent. So a weak loop leaves the lens handed in alone, right or 8% out, and correcting a
   lens it cannot see precisely is for a capture with the loops to do it, or for a lens a device
   keeps.

   Measured over two hundred seeds of noise at 0.4, 0.8 and 2 px on four shapes, and on the
   skipping ring with one pair thinned to fifteen matches at five times the noise, the least's error
   in these standard deviations spreads 0.73 to 1.01 and reached 3.1 at the most. The spread barely
   moves between seeds, which is what lets a threshold decide: a twelve-frame ring 0.007 to 0.036%,
   a two-by-four grid 0.07 to 0.38%, a triangle or a ring whose one loop skips a frame 0.39 to 2.8%,
   and the photograph ring's pairs — every detector, up to 1.5 px of noise added to them — 0.005 to
   0.14%. Handed the right lens, 800 noisy runs of the skipping ring and the triangle were all
   passed through and 200 of the grid at 0.8 px all fitted, the worst 0.39% out. The first version pooled the
   pairs' residuals outright, which is exact only where every edge's error moves alike with the
   scale; the solve puts a loop's misfit on its lightest edge, so a thin pair's noise counts for most
   where a pool dilutes it, and one such pair put the skipping ring 12.8% out, through the
   refutation since retired, in
   four seeds of two hundred. It also counted a pair measured both ways twice, reading the spread
   √2 tight (round 5).

   Two rules came before this one, and each was found wanting by a reviewer's Monte Carlo. The
   first asked the cost at both ends of the bracket to be four times the least, and a skipping ring
   at 0.8 px of noise — ORB's pair residual — passed it in forty seeds of forty, up to 1.5% out,
   with rotations ten times worse than the right lens gave and an edge error that read clean
   (round 3). The second asked the cost to double within half a percent of the least, and still
   took that ring in six seeds of two hundred, 2.2% out, while refusing the photograph ring's ORB
   fit in four seeds of ten once 1.5 px of noise was added, sending back a lens 5% long with the
   least within 0.25% (round 4). Both judged the least by the cost at it — how far the loops fail
   to close there — and that is a chi-square on the few degrees of freedom the loops leave,
   independent of the error that moved the least: it varied 140-fold across ten seeds while the
   least moved a quarter of a percent. Judging by it selects nothing.
   Where any of these fails — no loop among placed frames, a pair left with fewer than three
   matches, a trial not scored, no rise on both sides, not precise — `initial`
   comes back as given, every field of it. The loop check is not implied by the others: with it
   removed, an open eleven-pair chain was fitted to 502.7 of 500, because the priors — three degrees
   out, at a hundredth of an inlier each — pull hard enough to give the cost a minimum of their own,
   and it sits where their error puts it; that least rises on both sides and the precision rule
   does not refuse it either. The bracket is 0.7 to 1.4 times the
   focal length handed in, which is chosen rather than measured; what was measured is that every
   pair of the photograph ring is still accepted at 10% out.

   **A loop need not wrap the sphere.** The first version of this decision said a triangle turning
   about one axis closes under any focal length, since three angles that sum to zero still do when
   scaled, and its test asserted the triangle was passed through. It came back fitted, to 499.9967
   of 500, on exact matches: under a pinhole a turn moves a pixel by the tangent of its angle, not
   the angle, so under the wrong focal length a thirty-degree pair and a sixty-degree one are not
   scaled alike and the triangle stops closing. The first version of this paragraph said the pairs
   tilted; a reviewer's independent rebuild showed their axes stay exactly vertical. Its least is
   right, but a lone triangle is precise only to 0.39% or worse at 0.4 px of noise, and to 0.44%
   read at the half-pixel floor, so it is not taken — seeing the focal length and seeing it
   precisely are different questions, and the second is the one a capture asks. A grid of such
   loops, none wrapping, reads 0.07% and is.

4. **`GlobalSolution::lensFitted` says which happened.** Not `Intrinsics::estimated`, which the
   first version of this decision used: that field says whether a lens was ever estimated, and a lens
   a device kept from an earlier capture is an estimate this call did not make. So a lens passed
   through keeps `estimated` as it was given, and a fitted one has it set.

   **And `GlobalSolution::focalSpread` says how sure.** The least's standard deviation in log focal
   scale, reported wherever the search reached a least with the cost rising on both sides, taken or
   not, and infinite where it did not — an end of the bracket is no least.
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
  `GlobalSolution::lensFitted`, `GlobalSolution::focalSpread`) and in `engines/registration_engine.h` (`Refine`'s paragraph on the
  lens, and a refusal for matches that disagree with the inlier count). The TypeScript mirror moves with them: the generator
  mirrors every type in `types.h`, though no registration type crosses at runtime.
- **Memory.** Sixteen bytes a match as four floats, at most 500 a pair: 8 KB a pair, and a sphere of
  sixty frames registered against four neighbours each is about 2 MB of matches held until `Refine`
  returns. Small against the frames, but a new allocation per pair and now part of what a build
  holds.
- **Time.** `Refine` becomes twenty-four solves instead of one — the solve at the lens handed in,
  two to open the golden-section search, nineteen to narrow it to 1e-4 in log scale, and one half
  a percent either side of the best, which is where the precision is read. Each is cheap next to feature
  extraction, and how cheap on a phone is a measurement the WASM build will have to take once
  registration compiles there (ADR 0047).
- **An open capture keeps its guess.** A strip of frames that never closes a loop cannot estimate
  its focal length by this route and says so through `lensFitted`. A sphere closes loops everywhere,
  so this is a limit on partial captures rather than on the product.
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

**A width at the least: the cost must double within half a percent of it.** This decision's
second rule, and the one this paragraph once rejected the standard error in favour of, because a
count of independent loops seemed to be what a standard error needed and a ring's shared frames do
not give one. The count was never needed: the pairs' own residuals give the noise directly, and a
loop cannot absorb them. The width was judged against the cost at the least, which is the loops'
leftover disagreement — a statistic on two or three degrees of freedom, not the noise — and a
reviewer's probes showed it taking the skipping ring 2.2% out in six seeds of two hundred, and
refusing the photograph ring's ORB fit, within 0.25%, in four seeds of ten (round 4).
