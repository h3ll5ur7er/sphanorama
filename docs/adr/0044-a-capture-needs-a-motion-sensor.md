# 0044 — A capture needs a motion sensor, and says so when there is none

> **Supersedes [ADR 0042](0042-with-no-aim-coverage-decides-alone.md)** entirely, and narrows
> [ADR 0041](0041-aim-decides-which-cell-a-burst-belongs-to.md) by removing its one exemption.

## Context

ADR 0042 made the app work on a phone that reports no orientation: guidance targets by coverage,
`ArmBurst` declines to enforce a cone it has nothing to measure, and the user aims by eye. That was
the right answer to the question it was asked — *given that we support this device, what should it
do?* — and it took three rounds of review to get right, at three separate layers.

What none of that changes is what such a capture is worth. Cells fill in coverage order and the
pixels are whatever the user happened to be pointing at; nothing verifies that a cell's frames came
from that cell's direction. The sphere it produces is a folder of pictures with a plan's worth of
labels attached to them, and the labels are guesses. `RegistrationEngine` is what would make them
true — feature matching and bundle adjustment good enough to place a live stream of frames without
any external reference — and it is null. Getting there is far future and possibly never.

Meanwhile the degraded path is not free. It is a second set of rules through every layer that
decides anything: which cell to name, whether to enforce a cone, whether to offer a shutter,
whether a dwell can mature. Six of the findings across ADR 0041 and ADR 0042's reviews were
*about* that second set — a rule stated for the aimed case and applied to the blind one, or the
reverse. Twice a fix was correct for the phone with a sensor and wrong for the one without.

## Decision

**A capture requires a motion sensor. Without one the session is refused, and the page says what is
required and what is missing.**

- `ICaptureSessionManager::Begin` and `::Resume` refuse with `SensorUnavailable` when
  `IMotionSensorAccess::Capabilities()` reports `None`, before the camera is opened — asking for
  camera permission for a session that cannot start is the wrong order to fail in.
- `ICoveragePlannerEngine::Locate` keeps its unaimed branch, and it stops meaning what ADR 0042
  made it mean. Zero confidence is no longer a *device*; it is a session whose first reading has
  not arrived, or a stream carrying rates with no attitude in them. There is nothing to be inside
  of, so guidance seeks and no cell is named as held. What goes is the claim that this is a mode
  somebody can finish a sphere in.
- `ArmBurst` loses its exemption and always enforces the acceptance cone.
- The page loses `#capture` entirely, which completes the decision recorded on PR #44: the dwell
  fires every burst, and there is no second way.
- What the user gets instead is a sentence: this needs motion access, here is what is missing, and
  here is what to do about it.

## Consequences

- **A device that declined motion, or has none, cannot capture at all.** That is the point, and it
  is a real loss for a real user — an iPhone user who declines the permission prompt lands here.
  The message has to be good enough that they know it is a choice they made and can unmake.
- **One rule per question, everywhere.** `Locate` names the cell the camera is inside; `ArmBurst`
  enforces the cone; the dwell fires. The unaimed case is a session waiting for its first reading
  rather than a second way to capture, so nothing downstream of guidance branches on it:
  `HoldStill` needs a cell to be inside of, the dwell needs `HoldStill`, and `ArmBurst` refuses
  what it cannot verify. One path, with a start to it — which is why removing the branch in
  `Locate` outright was wrong, and was caught while implementing this: an unmeasured identity
  reads as an aim, `HoldStill` arrives, and on a rate-only stream the dwell matures and fires a
  burst at a cell nobody pointed at. That is the bug ADR 0041 exists to stop, arriving through
  the door this ADR was opening.
- **`PoseSample.confidence` still starts at zero** — the first ticks of a session arrive before the
  first sample — so the reticle still parks and guidance still says it is waiting. What is gone is
  the case where it *stays* zero for the life of a session.
- **`PoseMode::VisionOnly` is now unreachable from this manager.** It stays in the contract: it is
  what a vision-only capture would use the day `RegistrationEngine` can carry one, and this ADR is
  the record of why nothing selects it today.
- **Roughly two hundred lines of second-path reasoning come out**, along with the tests that pinned
  them. The ADRs stay: 0042's reasoning was correct and its measurements are the evidence for this
  decision, which is why it is superseded rather than deleted.
- **The bench and any replay client are unaffected**, because a recorded log carries orientation.

## Rejected alternative

**Keep the degraded path and warn.** Capture anyway, and tell the user the result may be poor. It
was rejected because the warning cannot be honest about the outcome: the failure is not "slightly
worse stitching", it is cells labelled with directions nobody measured, and it is invisible until
the build stage that cannot happen yet. A user who takes thirty-two bursts and gets nothing usable
has spent more than a user who was told at the start.

**Refuse in the page rather than the manager.** The page knows the capability and could decline
before calling. Rejected because a manager whose only defence is its client has no defence —
`Begin` is a contract call and the shell is not its only possible caller. And a second check in
the page would buy nothing: the manager's own runs before `camera_.Open`, so the refusal already
arrives before the permission prompt. The page's job here is to say it well, not to say it
first.
