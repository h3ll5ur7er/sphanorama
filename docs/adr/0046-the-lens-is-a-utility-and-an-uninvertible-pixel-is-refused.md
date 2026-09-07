# 0046 — The lens is a utility, and a pixel it cannot invert is refused

## Context

`Intrinsics` has been in `contracts/cpp/sphanorama/types.h` since the architecture was written. It
is passed to `ICoveragePlannerEngine::Plan` and to `IRegistrationEngine::Refine`, stored on
`CaptureSessionManager`, and written into every persisted session. Until this change **no code in
the repository read a field of it.** Both `Plan` implementations take the parameter unnamed and
tessellate from `CapturePlanSpec`'s field of view instead; `Refine` is null. `CaptureSessionManager`
fills in `width` and `height` from the camera and leaves the focal length and all five distortion
coefficients at zero, which is correct — the struct's own comment says a lens is *estimated during a
build*, and there has been no build.

Phase 2 is the phase that estimates it, and every stage of Phase 2 is this transform or its
inverse: rendering a synthetic frame from a known panorama, measuring a match residual in pixels,
seeding a rotation from a sensor prior, refining focal length and distortion in a bundle
adjustment, resampling into equirectangular output. So the projection comes before any of them and
all of them consume it.

Three things about it are easy to get wrong and invisible when wrong:

- **The Y axis flips.** Camera space is +Y up (ADR 0015, ADR 0017, and `Direction` in the utilities
  bar); a raster image is +y down. A model that is consistently upside down round-trips perfectly
  and stitches a sphere that is upside down.
- **Brown-Conrady has no closed-form inverse.** It is solved by iteration, and an iteration that
  has stopped moving has not necessarily stopped in the right place.
- **A distortion strong enough stops being invertible at all.** Where `r · radial(r²)` stops
  increasing the image folds back over itself, two directions land on one pixel, and there is no
  answer to give.

## Decision

**The lens is a utility — `core/src/utilities/camera_model.{h,cpp}` — not an engine**, and
**every way of having no answer is a refusal rather than a pixel.**

- Camera space is **−Z forward, +Y up, +X right**, the single frame §3.5 of the architecture
  already establishes. Image space is **+x right, +y down**, origin at the image's top-left corner.
- Distortion is **Brown-Conrady in OpenCV's parameter convention**, so `k1 k2 p1 p2 k3` measured by
  any other tool drops into `Intrinsics` unchanged.
- `Project` and `Unproject` answer a small struct carrying a `valid` flag. They refuse an unusable
  lens, a direction that is not a measurement, a direction at or behind the plane through the
  optical centre, and a radius past the fold — where the *derivative* of the radial map,
  `1 + 3k₁r² + 5k₂r⁴ + 7k₃r⁶`, has stopped being positive.
- `Unproject` iterates, then **checks its answer by calling `Project` on it** and refuses if the
  result does not land back on the pixel it was given. Not by repeating `Project`'s arithmetic
  locally: the fold refusal is a property of `Project`, and asking it is the only way the two are
  guaranteed to agree about where the fold is.
- `LensFromFieldOfView` is the only place intrinsics are *invented*. It exists to seed an estimate
  and to render synthetic datasets whose ground truth is known because the lens was chosen, and it
  does not set `estimated`.

## Consequences

- Phase 2 has its shared arithmetic, and one definition of it. The synthetic-dataset renderer, the
  registration engine, the coverage planner and the composition engine all measure angles the same
  way or they fail a test.
- A default `Intrinsics` — the one every capture session is holding right now — answers `false` to
  `IsUsableLens`, and a test says so by name. Nothing can begin quietly trusting a zero focal
  length without that test going red first.
- **The iteration budget is measured, not chosen.** On a `k₁ = −0.9` lens, a budget of 20 accepts
  pixels out to only 89.6% of the radius that genuinely has a preimage; 100 reaches 99.6% and 500
  reaches 99.98%. The loop exits as soon as it has settled — an ordinary phone lens takes about five
  passes — so 500 is a ceiling on the pathological case rather than the common cost. A smaller
  budget does not answer *approximately*; it refuses well-defined pixels near the edge of a wide
  lens and calls it a lens it cannot describe.

  It is 99.98% rather than 100% because the last sliver is the fold itself, where the iteration
  converges with a ratio going to 1 and no finite budget arrives. That sliver is also where the
  inverse is genuinely ill-conditioned — the forward map's derivative is heading for zero, so a
  pixel's worth of image spans an unbounded range of directions — and refusing it is the right
  answer rather than a rounding of one.
- The convergence check is load-bearing and was nearly not tested. Sweeping the model rather than
  reading it turned up the case that needs it: **strong tangential distortion**, which the fold
  check cannot reason about because `p1` and `p2` are not radial. There the fixed point settles
  somewhere that is not a preimage of the pixel at all, radial stays positive throughout, and
  projecting the answer gives a perfectly valid pixel — just not the one asked about.
- Cost: two conventions now have to be kept in step by hand — this model's, and whatever the
  synthetic-dataset generator uses to render. That is deliberate (see below) and it is why the
  agreement will be a pinned test rather than a shared function.

## Rejected

***A `LensEngine` behind a contract.*** Engines own a volatility axis and are swappable
implementations of a stated activity. Projection is neither: it is arithmetic that the registration
engine, the coverage planner, the composition engine and the bench tool all need, and an engine
that four engines called would be engines calling engines, which the layer rules forbid outright.
The volatility that *is* real here — which distortion model, how many coefficients — lives inside
`Intrinsics`, and a richer model changes that struct rather than adding a component.

***A NaN pixel to mean "no image".*** It reads well and it is the sentinel trap: NaN is producible
by the ordinary arithmetic in this file — a zero depth alone will do it — so it cannot also carry
the meaning "there was no answer". A caller could not tell a refusal from an overflow, and the one
place that matters is the one place nobody looks.

***A closed-form approximate inverse*** — fitting a polynomial to the inverse distortion, as some
pipelines do. Faster, and it never refuses. That is the objection: its error is silent,
lens-dependent, and largest exactly at the frame edge where the overlap between two cells lives, so
it would spend its accuracy where registration needs it most and say nothing about having done so.

***Sharing one implementation with the synthetic-dataset generator.*** Tempting — two copies of a
projection will drift. But a harness that renders its frames with the same code the engine under
test uses cannot measure that code: a wrong camera model would render frames consistent with itself
and score perfectly. Independent implementations are the point (§5.5 of the toolchain document says
the same about the reference), and the drift is handled by pinning their agreement in a test rather
than by removing one of them.
