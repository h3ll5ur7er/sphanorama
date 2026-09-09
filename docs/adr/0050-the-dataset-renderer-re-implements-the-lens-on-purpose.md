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
  `uv run --locked --group datasets` installs it, so the fourteen checkers keep the property ADR
  0048 gave them and one step pays for the group.
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
  flip, four orders over the tolerance. Widening it is still worth doing before the detector
  comparison leans on the geometry, but for coverage across lens families rather than because a
  convention error would slip through.

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

- **A lens that folds inside its own frame is now refused outright**, by `render_frame` and
  `write_dataset` both. The alternative is a dataset whose images have holes, and no colour can
  honestly stand for "no ray" — black is a colour the scene produces, and the byte a refusal used to
  write was 128, mid-grey, which an ordinary checkerboard pixel hits. Measured, the lenses a phone
  actually has are nowhere near this: at the corner of a 66x50 degree frame, k1 = -0.28 leaves the
  slope at +0.65 and a strong barrel at +0.55. Only a deliberately pathological lens is refused.

- **Checking the fold at the frame's outermost radius alone is not enough**, and that is the mistake
  ADR 0046 already made once. The slope `1 + 3*k1*u + 5*k2*u^2 + 7*k3*u^3` is a cubic in `u = r^2`,
  so it can dip below zero partway out and return positive by the corner: with k1 = -6.0, k2 = 5.5
  the endpoint reads a healthy +0.73 while the minimum inside is **-1.95**. The check evaluates the
  cubic's interior turning points too. It is belt-and-braces with the sampled Jacobian that follows
  it — either alone catches every lens that could be constructed here — and the closed form is kept
  because sampling a 33x33 grid is a hope about resolution rather than a proof.

- **Four more things had no guard at all**, each found by the arithmetic lens and each the same
  shape: a value that is not a measurement being answered rather than refused. A default
  `Intrinsics` maps the whole world onto pixel (0, 0) and reports every direction valid, because a
  constant map is its own inverse everywhere. An infinite depth is greater than zero, divides to the
  principal point, and answers the middle of the frame. A zero vector reaches `arctan2(0, -0)` and
  lands on the seam. And `Pose.rotate` did not normalise where `sphanorama::Rotate` does, so a
  quaternion one percent off unit turned a direction 0.69 degrees while `truth.json` recorded the
  rotation that was asked for — frames and ground truth disagreeing by more than the quantity the
  harness exists to measure.

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

***Porting `camera_model.cpp` line by line into Python.*** Independent in form and not in substance —
a transcription reproduces the original's misunderstandings faithfully, which is the one property
that must not carry over. Working from the published form and meeting the C++ at a hand-computed
number is what makes agreement mean something.

***Leaving ADR 0048's "visible in a lock file" protection as it was.*** That protection is spent:
numpy is in `uv.lock` now, so a future dependency added to the `datasets` group would not stand out
there. What actually keeps the checkers standard-library-only is the *position of one line* in
`ci.yml` — the dataset step runs after them, and a reviewer demonstrated that the identical checker
invocation placed after it imports numpy and exits 0. ADR 0048's decision stands and does not need
superseding; this is the erosion of one of its consequences, recorded here because nothing else
would say so.

***numpy in `dependencies`, not a group.*** Simpler to invoke and it puts a 16 MB wheel into every
CI job for the benefit of one step, undoing the property ADR 0048 was written to establish. The
group costs one flag at the call site, which is visible in `gate.sh` and in `ci.yml` where a reader
will meet it.

***Generating with OpenCV's `cv2.projectPoints` in Python.*** Genuinely independent of our core and
already trusted by ADR 0047's cross-check. Rejected because it makes the dataset's geometry depend
on a second large dependency in the tooling, and because OpenCV's inverse is the one thing ADR 0046
established we should *not* agree with — `cv::undistortPoints` runs five fixed-point passes and is
wrong on a wide lens, so a renderer built on it would bake that error into the ground truth.
