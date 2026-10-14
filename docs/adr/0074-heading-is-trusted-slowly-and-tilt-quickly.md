# ADR 0074 — The compass's heading is trusted slowly and gravity's tilt quickly

**Status:** accepted

## Context
A capture on a Pixel 9 Pro XL reported that in some directions the reticle kept moving with the
phone held still, which put frames in the wrong place and left gaps between them.

That is the signature of a compass, not of a gyroscope. `AbsoluteOrientationSensor` on Android is
the platform's rotation vector, which takes its heading from the magnetometer. Indoors the field is
bent by steel and wiring, differently in each direction, and the platform's fusion pulls its
heading towards the bent field over seconds — so a phone that has just turned into such a direction
and stopped sees the heading keep going.

[ADR 0024](0024-orientation-and-rates-are-fused-and-the-bias-is-state.md) fused that reading with
the gyroscope as one attitude: every part of the disagreement was corrected over 0.1 s and charged
to the gyroscope's offset over 0.5 s. Against a reading that creeps, both are wrong. The correction
follows it, so twenty degrees of creep across the two-second dwell that fires a burst arrived as
19.99° of reticle movement. The offset learns the creep's rate as a gyroscope error, so a dropout
afterwards turned a still phone by 9.94° in a second.

An attitude is two measurements with different failures. Its tilt comes from gravity, which an
accelerometer measures well and nothing indoors bends. Its heading, the turn about the world's up,
comes from the compass. Gravity cannot see that turn at all.

## Decision
**The disagreement between the gyroscope's prediction and the reading is split about the world's
up, and the two parts are trusted at different speeds.**

- **Tilt** is corrected over 0.1 s and charged to the gyroscope's offset over 0.5 s, as before.
- **Heading** is corrected over 10 s, so between readings the gyroscope carries it.
- **Heading is never charged to the offset.** A creeping compass would be learned as a gyroscope
  error. The offset's component about up is still learned, but only once the phone is turned so
  that component shows as a tilt, which is what sweeping a sphere does.

The split is about the **world's** up, mapped into the phone's frame, not about the phone's own
vertical. Pitched up, most of a heading error lies along the axis the camera looks down, and
splitting by the phone's axes would let that part through at the fast rate. A test pitched 60°
up fails if the split is made in the phone's axes.

Ten seconds is a starting point on the same footing as ADR 0024's two, not a measurement. It was
chosen against both ends of the failure:

- across a two-second dwell it lets through under a tenth of a creep;
- it brings a heading that has wandered back to the compass within a minute;
- a resumed capture is laid out against the compass, so a heading that never came back would put
  the second session's cells beside the first's rather than on them.

## Consequences
- **Creep.** Twenty degrees of compass creep across a two-second dwell moves the estimate 1.88°,
  against 19.99° before, at every elevation tested (0°, 60°, −45°).
- **Dropouts.** The creep is not learned, so the dropout after it moves the estimate by nothing,
  against 9.94° before.
- **Tilt is unchanged.** A 5° step in pitch, or in roll at level, is within 0.53° after half a
  second, the same figure as before.
- **Noise.** A reading 3° out on alternate samples comes out within 0.24° in tilt and 0.005° in
  heading.
- **Settling.** A heading the compass holds 10° away is reached to within 0.025° in a minute.
- **A roll is not purely a tilt.** At a phone pitched up, the share of a roll about the viewing axis
  that leans towards the world's up is a turn gravity cannot see. The compass corrects that share
  slowly, and the gyroscope follows it meanwhile. At the zenith the whole of a roll is heading. A
  test of a pitched roll failed while this was written for exactly this reason, which is why the
  roll case in `TiltIsStillCorrectedAsFastAsBefore` is level.
- **Heading now rests on the gyroscope's rates.** Before, an axis or unit mistake in
  `angularVelocityFromRotationRate` was corrected away within a tenth of a second. Now it would show
  as a heading that lags or overshoots a turn and then drifts back over ten seconds. The conversion
  is tested against the specification but has not met a device, and this is the first thing that
  would show if it is wrong.
- **An offset about up integrates through a dropout uncorrected** while the phone holds still. This
  assumes that offset is small. On Android the gyroscope behind `rotationRate` is meant to be the
  platform's drift-compensated one. That is not measured here.
- **Platforms without rates are unaffected.** An `OrientationOnly` stream takes every reading
  whole, so the creep reaches the reticle there exactly as before. Only a gyroscope can tell a creep
  from a turn.
- **No contract changes.** `PoseState::gyroBias` holds what it always held, now learned from tilt.

## Rejected alternatives
**`RelativeOrientationSensor` as the source, with the compass read once at the start.** This
removes the creep at the platform rather than in the core: Android's game rotation vector has no
magnetometer in it. It was rejected for three reasons:

- It needs two sensors running and a second attitude in `ImuSample`, which is a contract change.
- A resumed capture still needs the compass, and a heading read once in a bent field is wrong for
  the whole session.
- Fusing the two streams so that each covers the other's failure is the filter above with one more
  input.

[ADR 0017](0017-the-motion-port-reports-the-viewfinders-attitude.md) rejected it as a fallback for
the same reason: the plan is absolute.

**Gate the heading: refuse a reading whose heading disagrees by more than some angle.** This stops
a creep dead once it passes the gate, and lets it through untouched below the gate, so the
reticle's error is the gate. Worse, a session anchored in a bent field holds its wrong heading
against every true reading afterwards, because every true reading is outside the gate. A slow
correction degrades in proportion and always converges.

**Learn the heading's offset slowly rather than never.** Ten seconds to correct and fifty to learn
keeps ADR 0024's ratio, and by the continuous-time arithmetic its slower pole is 36 s. A heading
the gyroscope has wandered from would be absorbed partly as offset and then integrated into the
next dropout. The compass's error
is the larger one, so the offset it can teach is mostly its own creep.
