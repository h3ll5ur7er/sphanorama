# ADR 0017 — The motion port reports the viewfinder's attitude, as a quaternion

**Status:** accepted

## Context
Phase 0 was deployed and held in a hand, which found two things in the same place.

**The port reported the attitude of the chassis.** `RollBetween` measures roll from the camera's
+X axis — the device's right edge in its natural orientation — and the capture client draws that
roll on a screen whose "right" the browser re-defines when the user turns the phone. Held in
landscape, a perfectly level phone reads 90° of roll and the artificial horizon stands on end.
`screen.orientation` appeared nowhere in `shell/`, `bridge/` or `core/`.

**The source was an Euler triple whose singularity is this app's working pose.**
`DeviceOrientationEvent` reports intrinsic Z-X'-Y'' angles, and that parameterisation degenerates
at β = ±90 — the phone held upright, camera on the horizon, which is where a sphere is captured
from. There α and γ both act about the world vertical and neither is individually determined.
Simulating a roll about the camera axis through that pose gives gimbal lock at both ends and a
branch flip in the middle, where α jumps 90 → 270, β −5 → −150 and γ −90 → +90 between adjacent
samples. Feeding *exact* triples through the whole pipeline produces a perfectly continuous roll,
so the arithmetic is not the defect — a real sensor sitting on a singularity is, and no amount of
care in the conversion changes that.

The two are one decision because they are both answers to "what, exactly, is this port
reporting?", and the honest answer is neither the chassis nor a triple.

## Decision
An `OrientationSample` carries a `Quat`: **the viewfinder's attitude, already in the core's
frame**. The platform's angles do not leave the adapter.

Converting stays where ADR 0015 put it, and the adapter now also owns which way up the page is.
`toCoreFrame` post-multiplies by a rotation of −`screen.orientation.angle` about the axis out of
the display — which is the axis the camera looks along, so it may change the horizon and provably
cannot change the aim. That property is asserted rather than asserted-about: a test rotates the
screen through all four angles and requires the looking direction not to move. The angle is read
per sample, because the user turns the phone mid-session.

`AbsoluteOrientationSensor` is the preferred source and `deviceorientation` the fallback. The
sensor is constructed with `referenceFrame: 'device'` rather than `'screen'`, so both sources go
through one implementation of the screen rule and the tests can hold them to the same answer for
the same pose. It can fail two ways and only one is synchronous — the constructor throws where the
permissions policy forbids it, and `start()` reports a missing gyroscope through an error event
that may arrive at any point — so both land on the fallback, whenever they happen.

`MotionCapability` stays `OrientationOnly` for both. Neither source delivers rates, and that is
what the capability describes.

The client's sensor readout becomes azimuth, elevation and roll
(`shell/src/clients/capture/attitude.ts`). It had been showing α/β/γ, which no longer exist by the
time a sample reaches it, and the plan is written in azimuth and elevation anyway.

## Consequences
- A phone held in landscape has a level horizon, and the roll that crosses the contract is the
  roll of the picture rather than of the case around it.
- On Chromium the working pose is off the singularity entirely: a quaternion has no degenerate
  attitude.
- iOS has no Generic Sensor API, so Safari keeps the Euler path and its degeneracy. That is a
  platform limit, not a decision, and it is the argument for `DeviceMotionEvent.rotationRate` when
  someone measures whether it is usable.
- The permission gate still runs inside the user gesture that starts capture: the only platform
  that gates is the only platform with no sensor to try first, so it reaches the gated call
  synchronously. A sensor failing later falls back outside the gesture, on a platform that does
  not gate.
- The source can change mid-session, so the client names the live one next to the capability. On a
  phone that is the first thing worth knowing when the horizon looks wrong.
- `MotionSensorAccess` is a shell-internal interface, not a contract: `IMotionSensorAccess` and
  `ImuSample` are untouched, nothing was regenerated, and the core cannot tell which source ran.
- Reading `screen.orientation.angle` per sample is a property read at sensor rate, against a
  budget dominated by frames.

## Rejected alternative
**Rotate the overlay in the client by `screen.orientation.angle`.** One line, no adapter change,
and the horizon would look right immediately. It was rejected because the roll would still be
wrong *in the value that crosses the contract*: `CaptureGuidance.rollErrorDeg` would keep
describing the chassis, and the next consumer of the pose — frame registration, which cares about
the raster the camera actually produced — would inherit the same error with no hint of where it
came from. Correcting presentation over a wrong measurement is how a wrong measurement survives.

Two smaller ones. **`referenceFrame: 'screen'`** would have the platform apply the screen rotation
for the sensor path, leaving the rule in two implementations, one of which cannot be tested here.
**`RelativeOrientationSensor` as a second fallback** would cover a device with no magnetometer,
but it has no compass and the coverage plan is absolute, so its heading would drift away from the
sphere it is meant to be filling.
