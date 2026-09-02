# ADR 0005 — Use OpenCV algorithms piecemeal; do not use its `stitching` module

**Status:** accepted

## Context
OpenCV ships `cv::Stitcher`, which does the whole pipeline. It is tempting and it would produce a
panorama in a week.

## Decision
Depend on `core`, `imgproc`, `features2d`, `calib3d`, `photo` and `flann`, and call their
algorithms from behind our own `IRegistrationEngine` / `ICompositionEngine` contracts. Do not use
the `stitching` module as a unit.

## Consequences
- V7 (alignment) and V8 (composition) stay separately swappable instead of collapsing into one
  opaque dependency.
- Incremental rebuild (ADR 0004) is possible at all — `cv::Stitcher` has no notion of a partial
  re-run, and no way to inject a sensor prior or a mover mask into seam costs.
- We control the WASM binary size by linking a trimmed subset (budget: < 8 MB compressed).
- Cost: we write the orchestration ourselves, and we own the tuning.

## Rejected
*`cv::Stitcher` behind our contracts as a first implementation, replaced later.* The interfaces it
would satisfy are so much coarser than the ones we need that the "replacement" would be a rewrite,
and the intermediate version could not ship the differentiating features.
