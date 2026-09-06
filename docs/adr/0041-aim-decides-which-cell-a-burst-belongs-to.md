# ADR 0041 — Aim decides which cell a burst belongs to

## Context

Reported from a phone: point at a cell, press capture three times without moving, and the capture
fills the cell in front of you **and two neighbours**.

Two rules combined to produce it, and each was defensible alone.

`ICoveragePlannerEngine::Locate` named the nearest cell that was still a *hole*. Aiming at a cell
you had already shot named the nearest missing one instead — deliberately, and ADR-era reasoning
said why: naming the captured cell and saying "hold still" is an instruction to stand still and
re-photograph what you already have, and a person reading only the angular error cannot tell the
difference. A test asserted it.

`ICaptureSessionManager::ArmBurst` validated that the cell exists in the plan, that the frame count
and interval and settle are sane, and that no burst is already in flight. It did **not** validate
that the camera was pointing at the cell.

So: the first press satisfies the cell in front of you; on the very next tick the target moves to a
neighbour under a phone that has not turned; the second press arms against that neighbour; and the
burst records whatever the camera is looking at, which is still the first cell. The user sees three
cells fill. What is actually stored is worse than that — two of those cells hold **the wrong
pixels**, sharp and well-scored and filed under a direction they were not taken from. Nothing
downstream can detect it. The strip will rank them; the stitch will be wrong.

## Decision

**The cell the camera is inside is the cell guidance names, captured or not.** Aim beats coverage.
Outside every acceptance cone there is nothing to hold on, so the nearest *missing* cell is still
the answer and the capture keeps moving.

This reverses the earlier rule, and what that rule was protecting is kept by the **action** rather
than by the target: `HoldStill` on a cell that still needs shooting, `CellDone` on one that does
not. Nothing tells the user to re-shoot what they have; the cell under the reticle is simply named
honestly, which is also what makes a deliberate re-capture possible at all.

**`ArmBurst` refuses a cell the camera is not aimed at**, against the same acceptance cone the
planner guides with — so "the reticle is closed" and "this will arm" are one condition rather than
two that nearly agree. `FailedPrecondition`, because it is a true statement about the world that
the caller can fix by turning the phone.

**The page offers a capture only on `HoldStill`.** The core's refusal is the backstop, not the
first thing a user meets. `CellDone` is deliberately *not* offered yet: aiming at a captured cell
is how a re-capture will be asked for, and until a cell that is already captured looks different
from one that is not, a button that silently re-shoots it is worse than one that waits.

## Consequences

- **A burst can no longer be filed under a direction it was not taken from.** This is the whole
  point; everything else here is in service of it.
- **Pressing three times without moving now captures the same cell three times**, adding candidates
  to it, up to the per-cell cap of ADR 0037.
- **The pose engine tests could not aim.** `NullPoseEngine` pins the orientation to identity on
  every integrate, so with it the camera is permanently looking straight ahead — survivable while
  nothing read the pose, and not once arming depends on it. The manager tests gained an
  `AimablePoseEngine` a test can point, and a `TurnTo` helper, because `LookAt` alone changes
  nothing the manager can see: the pose is re-integrated only when a sample arrives.
- **Twelve tests armed at `plan.nodes.front()`**, which is the first cell of the first ring and has
  no reason to be the one under the camera. Their subject is spill, previews or coverage, so they
  now ask guidance which cell that is. One test — a burst that must not retarget mid-flight —
  staged itself by arming somewhere else, which is no longer possible and was never possible for a
  user either; it now arms where the camera is and *then* drifts, which is the case it was always
  about.
- **The dwell trigger has a decision waiting for it.** With an automatic trigger (PR #44), aiming
  at a cell for two seconds fires a burst — and `HoldStill` versus `CellDone` is exactly what stops
  a slow pan across finished cells from re-shooting all of them. That is the shape the trigger
  should be built against.
- **A cell captured from the very edge of its cone is still allowed.** The cone is the planner's
  own tolerance, so this adds no new judgement about what counts as aimed; if that tolerance is
  wrong, it is wrong in the reticle too, and both move together.
