# ADR 0041 — Aim decides which cell a burst belongs to

> **Narrowed by [ADR 0044](0044-a-capture-needs-a-motion-sensor.md).** The rule stands and is
> now unconditional: `ArmBurst`'s one exemption — standing its cone check down where nothing had
> anchored the pose — is gone, because the device it existed for is refused at `Begin`. What
> replaced it is a second condition rather than none: a burst is refused both when nothing has
> measured where the camera is pointing and when what was measured is outside the cell's cone.
>
> The other half of this ADR's decision is gone rather than narrowed, and it said it was recorded
> here and nowhere else: **"the page offers a capture only on `HoldStill`, through one predicate
> (`canCapture`)"**. There is no predicate and no offer — the dwell fires every burst (ADR 0043)
> and the button it gated was deleted by 0044. What survives of that half is its intent, in the
> core: `HoldStill` is still exactly the condition a burst arms on, and it is the manager that
> checks it now rather than a client that anticipates it.

## Context

Reported from a phone: point at a cell, press capture three times without moving, and the capture
fills the cell in front of you **and two neighbours**.

Two rules combined to produce it, and each was defensible alone.

`ICoveragePlannerEngine::Locate` named the nearest cell that was still a *hole*. Aiming at a cell
you had already shot named the nearest missing one instead — deliberately, and **ADR 0027** said
why: naming the captured cell and saying "hold still" is an instruction to stand still and
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

This **partly supersedes ADR 0027**, whose `SphereDone` half stands and whose targeting half does
not. What that rule was protecting is kept by the **action** rather than by the target:
`HoldStill` on a cell that still needs shooting, `AlreadyCaptured` on one that does not.

`AlreadyCaptured` is a new action rather than a reuse of `CellDone`, and the difference is not
cosmetic. `CellDone` is an *edge* — the contract says "`CellDone` on the tick that fills it", and
the manager's own private header is where "exactly once" is written down. Resting inside a captured cell's cone is a *level*, true on every
tick the phone stays there. Overloading one value for both turned the page's once-per-cell
`refreshCoverage()` into one per animation frame, which is what a reviewer caught before this
merged. Nothing tells the user to re-shoot what they have; the cell under the reticle is simply
named honestly, which is what makes a re-capture *discoverable from the viewfinder*. A deliberate
re-capture was already possible — `ICaptureSessionManager::RequestRetake` is written for it and
ADR 0037 keeps a cell's best eight so one competes — but only to a client that already knew which
cell to ask for. It has no client yet; naming the cell under the reticle is what will let one point
at it.

**`ArmBurst` refuses a cell the camera is not aimed at**, against the same acceptance cone the
planner guides with — so "the reticle is closed" and "this will arm" are one condition rather than
two that nearly agree. `FailedPrecondition`, because it is a true statement about the world that
the caller can fix by turning the phone.

**The page offers a capture only on `HoldStill`**, through one predicate (`canCapture`) rather than
a comparison inline in the render loop — because it is the same predicate a dwell trigger will fire
on. The core's refusal is the backstop, not the first thing a user meets. `AlreadyCaptured` is
deliberately *not* offered: aiming at a captured cell is how a re-capture will be asked for, and
until a cell that is already captured looks different from one that is not, a shutter that silently
re-shoots it is worse than one that waits.

## Consequences

- **A burst can no longer be filed under a direction it was not taken from.** This is the whole
  point; everything else here is in service of it.
- **Pressing three times without moving now captures the same cell three times**, adding candidates
  to it, up to the per-cell cap of ADR 0037.
- **The pose engine tests could not aim.** `NullPoseEngine` pins the orientation to identity on
  every integrate, so with it the camera is permanently looking straight ahead. The pose has been
  read on every tick since ADR 0027 — `OnMotion` hands it to `Locate` — so what changed is not that
  it started being read: it is that a **refusal** now turns on it, and a pose pinned to identity
  stopped being merely uninformative and became a gate. (Nothing ships with a pinned pose; the
  production wiring is `OrientationPoseEngine`.) The manager tests gained an `AimablePoseEngine` a
  test can point, and a `TurnTo` helper, because `LookAt` alone changes nothing the manager can
  see: the pose is re-integrated only when a sample arrives.
- **Twenty-four places armed at `plan.nodes.front()`**, which under the rings planner is the first
  cell of the first ring and has no reason to be the one under the camera. Their subject is spill,
  previews or coverage, so they now ask guidance which cell that is. (Under the null planner the
  single cell *is* at identity, so those were already aimed; they were changed for one rule rather
  than two.) One test — a burst that must not retarget mid-flight —
  staged itself by arming somewhere else, which is no longer possible and was never possible for a
  user either; it now arms where the camera is and *then* drifts, which is the case it was always
  about.
- **The page now offers capture only on `HoldStill`, which takes a decision PR #44 had left open.**
  That PR asks what a press should mean and lists disabling the button outside `Seek`/`HoldStill`
  as one answer; this change takes it, because the alternative is a button that reports a refusal
  the reticle already showed. Whichever of the two merges second has to reconcile the write-up —
  the decision belongs in one place, and it is here.
- **When a dwell trigger replaces the button**, `HoldStill` versus `AlreadyCaptured` is what stops a
  slow pan across finished cells from re-shooting all of them. That is the shape to build against.
- **A phone with no motion sensor can still *arm* every cell, and one line of the manager now knows
  it.** Arming was only half of it: guidance went on naming the one cell that sits at identity, so
  the page offered a capture for that cell and then nothing. The dead shutter had moved from the
  core to the client rather than gone, which a reviewer measured after this bullet was first
  written. ADR 0042 is the other half — with no aim, coverage decides the target — and the two
  should be read together. The gate applies only to a pose that was actually anchored, and it reads
  exactly one field to decide it: `PoseSample.confidence`, the contract's own way of saying "no
  reading has ever anchored this". It was briefly a pair with `PoseState::observed`, which is the
  wrong companion — that flag means "a sample arrived", which a rate-only stream satisfies at
  confidence zero. A
  device that declined motion or has none tracks vision-only and reports identity forever, so
  enforcing a cone against that number would refuse thirty-one cells of thirty-two and then leave
  the page with nothing to offer and nothing on screen saying why — measured on the shipped
  composition as `armable=1 refused=31`. UC-4 is a supported configuration, and such a user aims by
  eye, which is what vision-only means.

  The cost is that `docs/03-architecture.md` UC-4's *"no other component learns that sensors were
  absent"* is no longer literally true: `ArmBurst` reads the one field that says whether the pose
  is anchored. What it does with that is decline to have an opinion, so the *behaviour* UC-4
  promises is intact — every cell remains armable — but the sentence has been narrowed rather than
  kept, and both it and the manager's own copy of it now say so.
- **A stale pose is not caught.** The gate reads `pose_state_`, which `OnMotion` refreshes only on
  a non-empty batch. If the sensor stalls mid-session the pose freezes, and an arm is then
  *permitted* against a direction the camera has left — this ADR's own failure, reached through a
  stale pose instead of a stale target. It is not fixed here because it is not this call's
  question: the reticle the user is looking at freezes on exactly the same value, so a gate that
  disagreed with it would trade a wrong capture for a refusal the screen contradicts. Pose
  freshness is a session-wide property and wants one owner; `docs/06-roadmap.md` carries it.
- **A cell captured from the very edge of its cone is still allowed.** The cone is the planner's
  own tolerance, so this adds no new judgement about what counts as aimed; if that tolerance is
  wrong, it is wrong in the reticle too, and both move together.

## Rejected alternative

**Latch `targetNode` at press time and leave `Locate` alone.** The cheapest fix, and it is the one
PR #44 raises in its own words. It closes the race between deciding to press and the press landing
— the target cannot move out from under a finger — without reversing ADR 0027 and without a new
refusal on `ArmBurst`. Rejected for two reasons. It fixes only the race: a user whose aim was
already wrong when they decided still files this direction's pixels under that cell's name, which
is the half of the bug that actually corrupts the capture. And it leaves the reticle pointing at a
cell the camera is not on, so the moment a dwell trigger or an auto-shutter replaces the press,
the bug returns in full — the press is what latching protects, and the press is the part being
removed.

**Gate in the client only.** The page has held every cell's `acceptanceConeDeg` since before this
change and receives `angularErrorDeg` with each guidance answer; it already compares them to size
the reticle, so it could disable the button with no core change at all. Rejected because a manager whose only
defence is its client has no defence: `ArmBurst` is a public contract call, the shell is not its
only possible caller, and the failure it would let through is undetectable afterwards. The client
gate is *also* here — that is what `canCapture` is — but as the thing that keeps the refusal off
the user's path, not as the rule.

**Filter in the manager instead of the engine.** ADR 0027 rejected "let the manager filter" on the
grounds that "which cell does the user still need" is a coverage question owned by V4, and that
still holds: the target rule stays in `ICoveragePlannerEngine`. What is in the manager is the aim
check, which is a different question — not *which cell is needed* but *is the camera on the cell
this caller named* — and it has to be there, because only the manager can refuse the call.
