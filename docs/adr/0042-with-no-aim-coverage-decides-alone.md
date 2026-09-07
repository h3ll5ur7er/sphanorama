# ADR 0042 — With no aim, coverage decides alone

## Context

ADR 0041 made aim beat coverage: the cell the camera is inside is the cell guidance names, captured
or not. That fixed a real bug — a target moving out from under a still phone, filling neighbours
with the wrong pixels — and it is right whenever there is a camera direction to speak of.

There is a supported configuration where there is not. `IMotionSensorAccess.Capabilities()` reports
`none` when a phone has no sensors or the user declined them, which on iOS is the ordinary way to
land there. `CaptureSessionManager` then puts the pose engine into vision-only mode, where the
orientation is meant to come from frame-to-frame tracking — and `RegistrationEngine` is still null,
so nothing tracks anything. The pose is identity for the life of the session, at confidence zero,
which is the contract's own word for "nothing estimated this".

Aim-first against that number is not a weaker rule, it is a different one: it prefers whichever cell
happens to sit at identity, on every tick, for ever. Reviewers measured what that costs, one layer
at a time, and each layer looked fine on its own:

- `ArmBurst` enforced the acceptance cone against the unmeasured identity: **`armable=1 refused=31`**
  on the shipped composition. Fixed under ADR 0041 by exempting an unmeasured pose from the cone.
- With that fixed, `Locate` still named the identity cell, so the *page* offered a capture for one
  cell of thirty-two and then nothing: `canCapture` waits for `HoldStill`, and after that one burst
  guidance says `AlreadyCaptured` about the same cell for ever. The dead shutter had moved from the
  core to the client, not gone.
- And the pump only asks for guidance when samples arrive or a burst is running, so on a device that
  produces no samples the loop stopped evaluating at all after the burst.

Three separate defects, one cause: a rule stated for the aimed case and applied to a device that has
no aim.

## Decision

**`ICoveragePlannerEngine::Locate` takes a whole `PoseSample` rather than a `Quat`**, and prefers
the cell the camera is inside **only when `confidence` is greater than zero**. With no aim there is
nothing to put first, so coverage decides alone and the answer is ADR 0027's: the nearest cell that
is still missing. A sensorless capture then moves from cell to cell as it fills them, which is what
it did before ADR 0041 and what UC-4 describes.

The orientation is still used — it is what the reticle and the roll are drawn from — but it stops
being an argument about *which cell the user is at*.

**`canCapture` takes whether the aim is known**, and offers the coverage-named cell (`Seek`) when it
is not. What the page offers and what the core accepts still agree: the core has no cone to check
either, so neither of them is pretending to know something it does not.

**The render loop keeps ticking when there is no sensor.** It costs a facade round trip per frame on
that device, and it is the only signal such a capture has that anything moved.

## Consequences

- **UC-4 works again, and now has tests that say so by name.** `APhoneWithNoMotionSensorCanStillArmEveryCell`
  covers arming; `APhoneWithNoMotionSensorIsSentOnToTheNextCellAfterItCapturesOne` covers targeting,
  which is the half a sabotage of the aim rule did not catch — arming every cell is no use if
  guidance only ever names one of them.
- **The engine contract changed, and engine contracts are not mechanically checked.**
  `coverage_planner_engine.h` carries no `@boundary`/`@facade` marker, so `contract_gen.py` never
  reads it and there is no generated mirror to go stale. Nothing but review would have caught the
  header describing a rule the code no longer follows — which is exactly what happened to `Locate`
  once already on ADR 0041, and is why the header now states the confidence rule at length.
- **The manager test fixture is a phone that can aim.** It was built on `NullPoseEngine`, which
  reports confidence zero — a neutral choice before this ADR and a decisive one after it. Every test
  in that file would have been exercising the sensorless path while reading as though it exercised
  the normal one. `UnintegrablePoseEngine` needed the same treatment for the same reason: its point
  is a failing integrate, not an unmeasured pose.
- **Confidence is keyed on having an anchor, not on having moved.** `OrientationPoseEngine::Integrate`
  marked the state observed for any sample at all, so one carrying neither an attitude nor a measured
  rate left the orientation at identity and reported confidence 0.5 — and every rule above keys on
  that number. Splitting `observed` out fixed that for the first sample and left the same defect one
  sample later: dead reckoning *does* move the orientation, so a gyroscope-only stream reached its
  second sample at confidence 0.5 holding a heading integrated from an identity nobody had measured.
  A reviewer drove it against the shipped tessellation and `ArmBurst` accepted **zero of
  thirty-two** cells, with the page in aimed mode because `aimKnown` was true — this ADR's own
  failure, reached through the other door. The flag now asks whether the orientation *descends from
  a reading* (`PoseState::anchored`, set by the two branches that fold one in and never cleared), so
  dead reckoning after a reading is still 0.5 and dead reckoning from nowhere is 0.
- **A phone with a sensor is unaffected.** Confidence is above zero from the first real sample, so
  the aim rule applies exactly as ADR 0041 describes.
- **The blind capture is still blind.** Cells fill in coverage order and the pixels are whatever the
  user pointed at; nothing verifies they match. That is UC-4 as it has always been, and it is not
  fixed here — `RegistrationEngine` is what would fix it. What is fixed is that the app no longer
  stops after one cell.

## Rejected alternative

**Leave `Locate` alone and let the client special-case a sensorless device.** The page knows the
capability — it is what told the core `None` — so it could have named its own target. Rejected
because "which cell does the user still need" is a coverage question owned by V4, and ADR 0027
rejected exactly this for exactly that reason. A client deciding it would be a second answer to a
question the planner already answers, and the two would drift the first time the tessellation
changed.

**Pass a `bool aimed` beside the quaternion.** Smaller, and it reads as a flag the caller might
forget to set. `PoseSample` already carries the orientation and the confidence together *because
they are one claim* — splitting them at this call is what let the manager hand over an orientation
with the provenance stripped off in the first place.

**Keep the cone check off and leave targeting aim-first.** This was the state after ADR 0041, and it
is what the reviewers measured: arming succeeds and nothing is ever offered. Half a fix that reads
as a whole one is worse than neither, because the test that proves arming works passes.
