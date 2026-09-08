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

**The renderer is a second, independent implementation of the lens, and numpy arrives in an opt-in
dependency group.**

- `tools/synth_dataset.py` implements `project`, `unproject`, `lens_from_fov` and the
  equirectangular mapping in Python, from the same published Brown-Conrady form the core works
  from — not by calling the core, not by porting its code.
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

- **The independence is only worth what the cross-check is worth.** Right now that is one point,
  one lens, six coefficients — enough to catch a swapped `k2`/`k3` (7.5e-05 at a 1e-09 tolerance) or
  a swapped `p1`/`p2` (3.8e-03), which are the mistakes this convention actually invites. It is not
  enough to catch a shared misunderstanding of, say, which direction `yn` runs. Widening it is
  cheap — more points, more lenses, generated once and committed — and worth doing before the
  detector comparison leans on the numbers.

- **A tolerance in the wrong unit hid a 0.086-degree error.** The render test first asserted colour
  components to `atol=2e-3`, which sounds tight and is three orders of magnitude looser than the
  interpolation error it was meant to bound: measured, that is 2.3e-06 at a 2048x1024 panorama. A
  deliberately mis-rotated render passed it. The assertions are in **degrees** now, bounded at
  0.001, which the measured error clears by thirty times and which catches a 0.0011-degree error.
  The general rule is worth more than the fix: assert in the unit the artefact exists to serve, or
  the number's meaning has to be re-derived by every reader — and nobody will.

- **Two behaviours had no test until a sabotage found them.** Replacing the sampler's longitude wrap
  with a clamp left every test green, *including one written to look straight through the seam* —
  because wrapping and clamping differ on exactly one column, and a 64x48 frame lands on it only by
  luck. And removing `unproject`'s refusal entirely changed nothing, because every other test used
  a lens whose whole frame inverts. Both are now tested where the behaviour lives rather than where
  it was hoped to surface: the sampler directly, and a folding lens whose frame genuinely has
  pixels with no ray behind them.

- **A refused pixel renders black rather than guessed.** With a distortion strong enough to fold,
  part of the frame has no ray, and inventing one would put fabricated geometry into the ground
  truth everything downstream is measured against — the same rule ADR 0046 sets for the core, for
  the same reason.

- **The frames are uncompressed and the datasets are large.** A 640x480 frame is 900 KB, so a
  60-cell ring is 55 MB. `datasets/` is gitignored and regenerated rather than committed, so this
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

***numpy in `dependencies`, not a group.*** Simpler to invoke and it puts a 16 MB wheel into every
CI job for the benefit of one step, undoing the property ADR 0048 was written to establish. The
group costs one flag at the call site, which is visible in `gate.sh` and in `ci.yml` where a reader
will meet it.

***Generating with OpenCV's `cv2.projectPoints` in Python.*** Genuinely independent of our core and
already trusted by ADR 0047's cross-check. Rejected because it makes the dataset's geometry depend
on a second large dependency in the tooling, and because OpenCV's inverse is the one thing ADR 0046
established we should *not* agree with — `cv::undistortPoints` runs five fixed-point passes and is
wrong on a wide lens, so a renderer built on it would bake that error into the ground truth.
