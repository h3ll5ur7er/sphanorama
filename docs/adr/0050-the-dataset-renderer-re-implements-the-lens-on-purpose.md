# 0050 — The dataset renderer re-implements the lens on purpose, and numpy arrives in a group

**Status:** accepted

## Context

ADR 0049 built the thing that says how wrong a set of estimated rotations is. It has nothing to
score yet: that needs frames a phone would plausibly have captured, together with the rotation each
was taken at. `docs/05-toolchain-and-testing.md` §5.5 has described that tool since before there was
anything to render, and this is it.

Two questions had to be answered before any of it could be written, and neither is about rendering.

**The first is what the renderer projects with.** `core/src/utilities/camera_model` already does
exactly this arithmetic — direction to pixel through Brown-Conrady, and back — and calling it from
the tool would be the obvious economy. It is also the one thing that would quietly destroy the
harness. A dataset rendered *through* the code under test cancels any error the two share: get the
distortion convention wrong in both, and every frame is rendered wrong, registered wrong in exactly
the compensating way, and scored perfect. The harness would certify the broken projection. That is
the failure mode a measuring instrument cannot have, and it is invisible from inside — the tests
pass, the numbers look good, and nothing is measuring anything.

**The second is a dependency.** Rendering a panorama into frames is per-pixel work over millions of
pixels; pure-Python loops are not an option. ADR 0048 committed the tooling to `uv` with a lock
file, and said in its consequences that the checkers stay standard-library-only — they run on every
CI job, so a dependency there is a dependency on every build.

## Decision

**The renderer implements the lens itself rather than calling the core, and numpy arrives in an
opt-in dependency group.**

- `tools/synth_dataset.py` implements `project`, `unproject`, `lens_from_fov` and the
  equirectangular mapping in Python, in its own process and its own language, so a dataset is never
  rendered by the code that will be measured against it. It is **not** an independent
  re-derivation — see the first consequence, which a reviewer corrected — and what carries the
  weight is the pinning below rather than the separation.
- Both implementations are pinned to the **same hand-worked decimals**:
  `test_distortion_terms_are_opencvs_in_opencvs_order` here and
  `Project.TheDistortionTermsAreOpenCVsInOpenCVsOrder` in the C++ suite assert the same `xd` and
  `yd` for the same point and coefficients. Two implementations agreeing with an outside number is
  evidence; one implementation checked against itself is not.
- Conventions are mirrored exactly rather than chosen: camera space -Z forward, image space
  `cx = width / 2` (half a pixel from OpenCV's), rotations device -> world. A dataset expressed in a
  different frame from the core that reads it would be worse than no dataset.
- **numpy lives in a `datasets` dependency group**, not in `dependencies`. Measured rather than
  assumed: in a fresh environment `uv run --locked` leaves numpy absent and
  `uv run --locked --group datasets` installs it, so the checkers keep the property ADR 0048 gave
  them and one step pays for the group. (This said "the fourteen checkers"; the count is quoted in
  three places in this repository and matches nothing countable — six checker scripts, seven
  invocations, eight test suites, thirteen steps — so it is dropped rather than replaced.)
- Output is **binary Netpbm (P6)** and a `truth.json` carrying the rotation, the lens and the
  conventions. A P6 file is a header and the pixels; any consumer reads it in a dozen lines and
  none needs a library.

## Consequences

- **"Independent" was too strong a word, and a reviewer was right to press it.** What was written
  is not a re-derivation: `_distort` is term-for-term the core's `DistortAt`, the Jacobian and the
  Newton step are the same expressions under the same names, and one comment was copied verbatim.
  That is the thing the *Rejected* section below says defeats the purpose — and it kept the
  arithmetic while dropping the guards, which is the worst of both positions and is exactly how the
  fold defect below happened.

  What is genuinely independent is narrower and worth stating precisely: the term-by-term form was
  taken from the published Brown-Conrady definition, and both implementations are pinned to
  hand-worked decimals nobody derived from either of them. That pinning is what has value. The
  claim in this ADR and in the module docstring now says that rather than "independent
  implementation".

- **The cross-check is one point, one lens, five coefficients**, and is stronger than this ADR first
  credited. It catches a swapped `k2`/`k3` (7.5e-05 against a 1e-09 tolerance) and a swapped
  `p1`/`p2` (3.8e-03) — and also a flipped `yn`, which the first version of this bullet named as the
  example it *could not* catch: the tangential term `2*p1*xn*yn` moves `xd` by 0.012 under a sign
  flip, four orders over the tolerance.

  **Widened, and the widening earned itself.** It is fifteen points over five lens families now — no
  distortion, a typical phone, a strong barrel, a pincushion and one the tangential terms dominate —
  across three sign quadrants, with both suites asserting the same decimals computed from the
  published form in exact decimal arithmetic. The single point remains and is still the sharper
  instrument for a coefficient swap. What it could not see is a mutation that is invisible in one
  quadrant: replacing `xn` with `abs(xn)` in the tangential cross term passes the single point
  exactly, because that point has both coordinates positive, and fails the table. That is the whole
  argument for coverage rather than sharpness, and it is a measured example rather than a worry.

- **The rotation convention had a written promise and nothing executable behind it, and now has
  both.** The module docstring said a pose written in the generator names the same direction the
  coverage planner would. Each side was pinned to hand-derived vectors *separately*, which catches a
  mistake in one implementation and not a convention both share — and a shared rotation-convention
  error is the worst case this ADR exists to prevent, since a dataset rendered in the wrong one
  registers wrong in exactly the compensating way and scores perfect.

  Three azimuth/elevation pairs, both the forward axis and the camera's +X, asserted against the
  same decimals in `tools/test_synth_dataset.py` and `core/test/utilities/quaternion_test.cpp`. The
  +X row is what makes it more than a restatement of `Direction`: a convention with forward right
  and roll wrong passes on forward alone.

  **The first version of this bullet claimed the C++ suite could not detect a reversed composition
  order at all, and that was false.** Reverting `Multiply(yaw, pitch)` fails five tests: the new one
  and four `CoveragePlanner` tests that predate this work. The error was in how it was measured — I
  ran the suite under `--gtest_filter='FromAzimuthElevation.*:Rotate.*'`, saw only the new test
  fail, and wrote a sentence about *the suite*. Constraining an observation and then stating the
  conclusion unconstrained is the second time that shape of mistake has been caught on this project
  by someone re-running the measurement rather than reading the code.

  What is true is narrower and still worth the test: the four that catch it diagnose it as a
  coverage count and a wrong cell id — `EveryDirectionOnTheSphereIsInsideSomeCell` reports 270 of
  4000 directions uncovered — which says the planner is broken and not which convention moved. The
  four *pre-existing* tests of `FromAzimuthElevation` itself all pass under it — and **why** each is
  blind is worth more than their number, which this bullet has now got wrong twice:

  - `PointsForwardAtTheOrigin` — (0, 0), so both factors are the identity.
  - `ElevationLooksUpAndDown` — azimuth 0, so the yaw factor is the identity.
  - `AzimuthSweepsTheHorizon` — elevation 0, so the pitch factor is the identity.
  - `IsPeriodicInAzimuth` — compares the function against itself, which is self-consistent under
    *any* composition order.

  Three pure-axis cases where one factor vanishes, and one self-comparison. Only a case with both
  angles non-zero can see the order at all, which is exactly what the Python twin already says at
  `test_an_azimuth_turns_about_up_and_an_elevation_lifts_toward_it`. So the value is a direct
  diagnosis where there was only an indirect one, and that is a smaller claim than the one first
  made here.

  **And the parallel claim about the Python side was false too**, asserted in the same breath and
  never run. `main`'s 77-test Python suite *does* catch a reversed composition order, through
  `test_an_azimuth_turns_about_up_and_an_elevation_lifts_toward_it` — a test written in an earlier
  round precisely because pure azimuth and pure elevation cases do not separate the two orders. So
  the asymmetry this bullet describes is C++-side only. Three claims of mine in this ADR have now
  been corrected by someone re-running a measurement, and all three failed the same way: a result
  measured under some restriction, and then stated without it.

- **A tolerance in the wrong unit hid a 0.086-degree error.** The render test first asserted colour
  components to `atol=2e-3`, which sounds tight and is 868 times looser than the interpolation error
  it was meant to bound: measured, that is 2.30513e-06 in colour components at a 2048x1024 panorama.
  A deliberately mis-rotated render passed it. The assertions are in **degrees** now, bounded at
  0.001; the same interpolation error expressed in that unit is 2.54419e-05, so the bound clears it
  by 39 times and catches a 0.0011-degree error. The two numbers are in different units and the
  first version of this bullet compared them as though they were not.
  The general rule is worth more than the fix: assert in the unit the artefact exists to serve, or
  the number's meaning has to be re-derived by every reader — and nobody will.

- **Two behaviours had no test until a sabotage found them.** Replacing the sampler's longitude wrap
  with a clamp left every test green, *including one written to look straight through the seam* —
  because wrapping and clamping differ on exactly one column, and a 64x48 frame lands on it only by
  luck. And removing `unproject`'s refusal entirely changed nothing, because every other test used
  a lens whose whole frame inverts. Both are now tested where the behaviour lives rather than where
  it was hoped to surface: the sampler directly, and a folding lens whose frame genuinely has
  pixels with no ray behind them.

- **The first version of this ADR claimed a refusal that did not happen.** It said a refused pixel
  renders black rather than guessed. There was no fold test anywhere in the file — `usable` read
  `abs(determinant) > 1e-15` where the core requires `radial > 0 && determinant > 0` — so the
  solver crossed the fold freely and **468 of 3072 grid pixels came back with fabricated
  directions**, the frame corner among them, answered 52 degrees off axis pointing the opposite way
  to the truth. Two reviewers found it independently from different directions.

  The round trip could not catch it, and ADR 0046 says why in as many words: past the fold `radial`
  goes negative, which flips the sign of `xd`, so the wrong answer really does reproject onto the
  pixel it was asked about. A round-trip check is structurally blind to the wrong preimage. Both
  tests written to pin this behaviour derived their expectation from the same missing guard, so
  both passed while it happened.

  The fix is three things, and no one of them is individually necessary on the inputs available —
  which is worth recording rather than pretending one line is load-bearing. The starting guess is
  pulled toward the optical centre until the map is defined there; each damped step is accepted only
  if the trial point is *both* defined and strictly closer; and the answer is checked against
  `defined_at` before it is returned. Measured: 468 fabricated answers become 0, and the count of
  genuinely answered pixels falls from 2104 to 1636, which is the honest number for that lens.
  (Round 2 showed that `defined_at` is the wrong test to end on — see below. These numbers stand as
  what was measured at the time, which is what a consequences section is for.)

- **A lens with a rayless pixel is refused, and the frame is the check.** No colour can honestly
  stand for "no ray" — black is a colour the scene produces, and the byte a refusal used to write
  was 128, mid-grey, which an ordinary checkerboard pixel hits.

  How that refusal is *decided* is the part this ADR got wrong twice, and the correction is below.

- **The whole-lens fold check was wrong in both directions and is gone rather than fixed.** Two
  round-2 reviewers found it independently. `lens_folds_in_frame` took the frame corner's
  **distorted** normalised radius — `(pixel - c) / f`, which is the quantity `unproject` itself
  calls `xd` — and fed it to a slope cubic and a sampled Jacobian that are functions of the
  **undistorted** radius. It was answering a question about the wrong interval. Measured:
  `k1 = -1, k2 = 0.3` passed while 48,108 of 307,200 pixels have no ray, and `k1 = 2, k2 = -1` was
  refused while every one of its 307,200 pixels is answerable.

  It is deleted, not repaired. Asking `unproject` about every pixel of the actual frame is exact
  where a 33x33 grid was a hope about resolution, and it is work the render does anyway. **The core
  has no whole-lens check either** — which should have been the clue that inventing one was the
  wrong move, and is the fourth consequence in this file of the same root cause.

- **The guards were ported halfway, and the half that was left out is the one that matters.**
  `defined_at` is `DefinedAt`: a *pointwise* test, true when the map is well behaved at a point. The
  core never uses it as the answer. `Unproject` and `Project` both ask
  `DistortionInvertsAlongTheRay` — exact radial via `RadialMapIncreasesUpTo`, plus 64 samples along
  the ray for the tangential terms that do not reduce to one dimension — because a solution that
  *leapt* the fold and landed somewhere calm on the far side satisfies the pointwise test and is
  still the wrong preimage.

  Measured on `k1 = -1, k2 = 0.3`: **36,036 pixels answered from the far side**, the frame corner
  among them at r = 1.5832 where the near branch is 0.6499 — 24.7 degrees apart, both genuine
  preimages, `valid = True` on the wrong one. The round trip cannot separate them and this ADR
  already said why. `project` had no fold guard at all, which made it worse than useless as the
  adjudicator `unproject` trusts: the two agreed by sharing a blind spot.

  Both are ported now, and `radial_map_increases_up_to` carries the core's refusal on a non-finite
  discriminant, which had also been dropped — `>= 0.0` is false on a NaN, which is the same branch
  as "there are no real roots", and they are not the same thing.

- **The fix for the fold was itself untested, and that is the finding worth keeping.** A reviewer
  replaced `defined_at` with `return True` and all 40 tests stayed green. The test written for it
  filtered with `defined_at` inside `unproject` and then asserted `defined_at` on the survivors — so
  the function was its own oracle. Every arm of the fix was individually deletable; three removed
  together produced a bit-identical mask.

  That is the same sentence this repository wrote down one round earlier about `Pose.rotate`, whose
  defect was that every test computed its expectation by calling it. **It was written, and then
  committed again inside the fix for it.** The tests are judged against a brute-force near-branch
  solve now — far too slow to ship, calling nothing under test, which is what makes it an oracle.

- **The reassurance about phone lenses was wrong, and its numbers silently needed a coefficient the
  sentence omitted.** This ADR said k1 = -0.28 leaves the corner slope at +0.65 and that only a
  deliberately pathological lens is refused. Both halves fail: +0.65 requires the `k2` beside it,
  and with `k1 = -0.28` *alone* a 66x50 degree frame's corner is past the fold — the largest
  distorted radius the lens can produce is 0.7275 and the corner sits at 0.8104, so that pixel has
  no preimage on either branch. Refusing it is correct; claiming it would never happen was not.

  The true statement is narrower and worth having: a real calibration's positive `k2` is what pulls
  the fold outside the frame, and a `k1` quoted without one says nothing. The suite's lens list hid
  this because every negative `k1` in it was paired with a positive `k2`.

- **Four more things had no guard at all**, each found by the arithmetic lens and each the same
  shape: a value that is not a measurement being answered rather than refused. A default
  `Intrinsics` maps the whole world onto pixel (0, 0) and reports every direction valid, because a
  constant map is its own inverse everywhere. An infinite depth is greater than zero, divides to the
  principal point, and answers the middle of the frame. A zero vector reaches `arctan2(0, -0)` and
  lands on the seam. And `Pose.rotate` did not normalise where `sphanorama::Rotate` does, so a
  quaternion one percent off unit turned a direction 0.69 degrees while `truth.json` recorded the
  rotation that was asked for — frames and ground truth disagreeing by more than the quantity the
  harness exists to measure.

- **The fix for the fold had an ordering defect of its own, and self-review rather than a reviewer
  found it.** The refusal, the guard in `direction_to_equirect` and the `defined_at` check all
  arrived in one commit, and nothing considered the order between them: `render_frame` read `valid`
  *after* rotating the directions and mapping them to panorama coordinates. A refused row's
  direction can be non-finite, so the frame was diagnosed one line too early — as "a zero or
  non-finite vector names no direction", which is true of that row and the wrong statement about
  the frame, naming a direction where the problem is a lens, and losing the count of how many
  pixels are affected. The check moved ahead of both consumers.

  Worth recording because it is the third time in this repository that a fix was the defect. The
  test for it forces the state rather than finding it — no lens whose frame is answerable
  throughout is known to leave a NaN there, which is exactly why the raise is a backstop — so it
  substitutes an
  `unproject` that refuses one row. Removing the raise entirely fails that test and nothing else,
  which is the honest description of a backstop: one test stands on that path, and it is there
  because the path is reachable in principle rather than because it has been reached.

- **Brown-Conrady was written out four times and is written once now.** `_distort`, `defined_at`,
  the Newton residual and the backtracking trial each rebuilt it, and the Jacobian appeared twice
  more. The core hit this first and consolidated to one `DistortAt` returning a struct, with a
  comment on `Project`'s use of it saying why: that is the copy which *adjudicates* the solver's
  answer, so of the places the arithmetic lived it was the one that could least afford to drift.
  The port copied the expressions and not the lesson. `distort_at` is that struct here, and the
  consolidation is bit-for-bit: the same accepted count and the same hash of directions and of a
  rendered frame, before and after.

- **The renderer costs about 430 MiB and 11 seconds per megapixel, so it is a 640x480 tool.**
  Measured on one frame, and linear: 0.3 MP takes 2.6 s and peaks at 159 MiB, 1.2 MP takes 12.7 s
  and 550 MiB, 3.0 MP takes 32.6 s and 1,305 MiB. Extrapolated to a 4000x3000 phone frame that is
  roughly 5,200 MiB and 130 s, which a reviewer measured directly at 5,111 MiB and 126-137 s. A
  60-frame sphere at that resolution would be about two hours, and the list of rendered frames
  alone — held whole so a dataset is all-or-nothing — would be 2.16 GB rather than the 55.3 MB it
  is at 640x480.

  Nothing needs it: registration accuracy is measured in degrees and does not want full-resolution
  frames. It is recorded because the number is not guessable from the code and because the two
  round-3 reviewers reported figures that look contradictory and are not — one measured the
  all-or-nothing list (55.3 MB, correct), the other a single 12 MP render's peak (5,111 MiB, also
  correct). They are different quantities, and a reader meeting both without this note would try to
  reconcile them.

- **The frames are uncompressed and the datasets are large.** A 640x480 frame is 921,615 bytes, so
  a 60-cell ring measures 55.3 MB on disk. `datasets/` is gitignored and regenerated rather than committed, so this
  is disk rather than repository weight; if it becomes a nuisance, PNG through `zlib` is about
  thirty lines and no new dependency.

- **What is deliberately not here.** Noise, blur, rolling-shutter skew, exposure ramps, bursts per
  cell and composited movers are all named in §5.5 and none is built. Each is a modifier over this
  geometry and each deserves its own increment with its own invariant; stacking them into the first
  commit would mean debugging the projection through six other things. The panorama is procedural
  for the same reason — a checkerboard with a sine wash, enough texture for features to exist,
  no asset to fetch and nothing to make the tests non-deterministic.

## Rejected

***Calling the core's `camera_model` through a binding.*** One implementation, no drift, and it
defeats the purpose: the dataset would be rendered by the code the dataset exists to measure, and
a shared error in the distortion convention would cancel exactly. The whole value of the harness is
that it can disagree with the core.

***Porting `camera_model.cpp` line by line into Python.*** **This is what happened, and leaving it
in the rejected list was a claim contradicted by this ADR's own first consequence.** A reviewer
pressed it in round 2 and was right.

The original argument was that a transcription reproduces the original's misunderstandings
faithfully. That is true, and it is precisely what went wrong: the port kept the arithmetic and
dropped the guards, which is worse than either a clean port or a genuine re-derivation. Round 2
then found that the *repair* had repeated it one level down — `DefinedAt` ported, and
`RadialMapIncreasesUpTo`, `DistortionInvertsAlongTheRay` and `IsUsableLens`'s centre rule not.

So the position has moved rather than been restated. Porting is what this file does; what makes
agreement mean something is not the manner of writing it but the pinning to hand-worked decimals
neither implementation derived, and the discipline that a port is *complete or not attempted* —
half a port is the failure mode this ADR has now recorded three times.

***Leaving ADR 0048's "visible in a lock file" protection as it was.*** That protection is spent:
numpy is in `uv.lock` now, so a future dependency added to the `datasets` group would not stand out
there. What actually keeps the checkers standard-library-only is the *position of one line* in
`ci.yml` — the dataset step runs after them, and a reviewer demonstrated that the identical checker
invocation placed after it imports numpy and exits 0. ADR 0048's decision stands and does not need
superseding; this is the erosion of one of its consequences, recorded here because nothing else
would say so.

***Leaving ADR 0046's "independent implementations are the point" premise unmarked.*** ADR 0046
rejected sharing one implementation with this generator on the grounds that independent ones are
what give agreement meaning. That premise is spent in the same way ADR 0048's lock-file protection
is, and for the same reason recorded above: these implementations are not independent, so what
0046 was buying is not what it got. It is noted here rather than in 0046, and 0046 is not
superseded — its decision (the lens is a utility, an uninvertible pixel is refused) stands
untouched and is if anything strengthened by round 2, which found this file wrong in exactly the
places it had not copied 0046's guards. This ADR recorded 0048's erosion because nothing else
would say so; the same argument applies here, and omitting it while including the other was the
inconsistency a reviewer named.

***numpy in `dependencies`, not a group.*** Simpler to invoke and it puts a 16 MB wheel into every
CI job for the benefit of one step, undoing the property ADR 0048 was written to establish. The
group costs one flag at the call site, which is visible in `gate.sh` and in `ci.yml` where a reader
will meet it.

***Generating with OpenCV's `cv2.projectPoints` in Python.*** Genuinely independent of our core and
already trusted by ADR 0047's cross-check. Rejected because it makes the dataset's geometry depend
on a second large dependency in the tooling, and because OpenCV's inverse is the one thing ADR 0046
established we should *not* agree with — `cv::undistortPoints` runs five fixed-point passes and is
wrong on a wide lens, so a renderer built on it would bake that error into the ground truth.
