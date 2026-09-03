# ADR 0004 — A build is a fingerprinted DAG, not a pipeline run

**Status:** accepted

## Context
"Retake this region" is the feature that most distinguishes this product. Implemented over a batch
stitcher it means re-running the entire stitch for a one-cell change — on a phone, for a sphere
that took minutes to build.

## Decision
Model a build as a DAG whose nodes (features, pairwise edges, global solve, seams, tiles) are keyed
by a fingerprint of their inputs: content hash of the selected frame, pose, parameters and engine
version. `PanoramaBuildManager.Invalidate(dirtyNodes)` recomputes only the transitive closure
downstream of the change.

## Consequences
- A retake re-runs one cell's features, a handful of pairwise edges, the (cheap) global solve, and
  the affected tiles.
- Stage outputs are cacheable across a page reload because fingerprints are stable.
- Blending must be tiled, which it should be anyway for memory reasons.
- The global solve stays a full recompute deliberately: it is milliseconds, and partial solves
  would bake in drift.
- Cost: fingerprint discipline. Every engine must declare a version that participates in the key,
  or stale results survive a code change.

## Rejected
*Re-stitch everything and show a progress bar.* Simple, and it makes the product's best feature
feel like a punishment.
