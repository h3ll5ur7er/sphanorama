# ADR 0003 — A capture cell owns a set of candidates, not a frame

**Status:** accepted

## Context
Photo Sphere kept one frame per reticle. A blink, a bump or a passing pedestrian permanently
poisoned that cell. We want a burst — but a burst is only worth capturing if the extra frames
survive to where they are useful.

## Decision
The session document stores every burst candidate with its pose and quality score. Selection is a
separate, swappable engine (`IFrameQualityEngine`), and the *selection*, not the frame, is what
feeds a build. Candidates survive into the build stage.

## Consequences
- Automatic selection can be overridden by the user; the override uses the same dirty-node path as
  a retake, so one mechanism serves two features.
- Intra-cell candidate disagreement becomes a direct mover/parallax signal
  ([docs/04 §4.5](../04-runtime-topology.md)) — ghost detection falls out of the burst rather than
  requiring separate machinery.
- Cost: storage. Mitigated by `IFrameStoreAccess` residency tiers — only the selected candidate is
  ever held at full resolution in the heap; the rest rest encoded or spilled to OPFS.

## Rejected
*Collapse the burst at capture time.* Cheaper in storage, but it throws away the exact signal that
makes ghost detection and manual re-selection possible.
