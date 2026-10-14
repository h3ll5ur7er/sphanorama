# ADR 0074 — The compass's heading is trusted slowly and gravity's tilt quickly

**Status:** accepted

## Context
A capture on a Pixel 9 Pro XL reported that in some directions the reticle kept moving with the
phone held still, which put frames in the wrong place and left gaps between them.

What the report establishes is narrower than its cause. With the phone still, the filter followed
the *reading* within a tenth of a second whatever the rates said, so the reading moved. It is
consistent with a compass, and that is the leading candidate rather than a finding: nothing was
logged that would tell a bent field from the platform's own fusion settling, or say which source
was live. A still-phone log in which the reading's heading moves while `rotationRate` stays near
zero, with the source named, would confirm it. `AbsoluteOrientationSensor` on Android is
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
- **Heading is charged to the offset over 50 s**, five times its correction time as tilt's is, and
  from at most 0.05 rad of disagreement. That cap is what an offset of 0.005 rad/s, the most a
  calibrated gyroscope plausibly carries, stands away from the compass at ten seconds' correction.
  A larger disagreement is the compass jumping, not the gyroscope drifting.

The split is about the **world's** up, mapped into the phone's frame, not about the phone's own
vertical. Pitched up, most of a heading error lies along the axis the camera looks down, and
splitting by the phone's axes would let that part through at the fast rate. A test pitched 60°
up fails if the split is made in the phone's axes.

It is a **twist and a swing**, not a rotation vector projected onto up. The twist is the
disagreement's rotation about up; the swing is what remains, and it is exactly the disagreement
about where gravity points whatever the heading. A projection agrees for small disagreements and
not for large ones: it carries part of a small tilt along with a large heading and turns the rest
sideways, and the fast loop then charges the gyroscope for a tilt that is not there.

Ten seconds is a starting point on the same footing as ADR 0024's two, not a measurement. It was
chosen against both ends of the failure:

- across a two-second dwell it lets through under a tenth of a creep;
- a compass that jumps while readings keep arriving, and a gyroscope offset about up, are both
  corrected at this speed, so slower leaves either standing longer;
- a resumed capture starts on the compass, taking its first reading whole, and so does the first
  reading after a dropout. Neither reaches this constant. What a resume needs from it is that the
  first session's heading stayed near the compass, so that the second session's cells land on the
  first's. This holds while the live source is `AbsoluteOrientationSensor`. The
  `deviceorientation` fallback is relative on Chrome for Android, so nothing there is laid out
  against a compass, and nothing there can creep either.

## Consequences
- **Creep.** Twenty degrees of compass creep across a two-second dwell moves the estimate 1.89°,
  against 19.99° before, at every elevation tested (0°, 60°, −45°).
- **Dropouts.** The creep teaches the offset almost nothing, so the dropout after it moves the
  estimate 0.011°, against 9.94° before.
- **Only within uninterrupted fusion.** A creep still in progress when a dropout ends arrives
  whole, because the first reading after a dropout is taken as it stands.
- **Tilt is unchanged.** A 5° step in pitch, or in roll at level, is within 0.53° after half a
  second, the same figure as before.
- **Noise.** A reading 3° out on alternate samples comes out within 0.245° in tilt and 0.005° in
  heading.
- **Settling.** A heading the compass holds 10° away is within 0.67° after a minute and 0.031°
  after three. A jump of up to 175° beside a 2° tilt settles to within 0.005° in five minutes, and
  is never more than 1.8° out after the first forty seconds.
- **An offset about up is learned slowly, and shows while it is.** At 0.005 rad/s the estimate
  peaks 2.2° out after about twenty seconds, is 1.1° out at a minute and 0.23° at two. At
  0.02 rad/s, past the cap, it peaks at 9.8° and takes five minutes. ADR 0024 learned either within
  seconds. A sweep round the horizon does not speed this up, because turning about up leaves up
  where it was in the phone's frame.
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
- **The cap assumes a calibrated gyroscope.** On Android the gyroscope behind `rotationRate` is
  meant to be the platform's drift-compensated one. That is not measured here; an offset well past
  0.005 rad/s is learned, but slowly.
- **Platforms without rates are unaffected.** An `OrientationOnly` stream takes every reading
  whole, so the creep reaches the reticle there exactly as before. Only a gyroscope can tell a creep
  from a turn.
- **No contract changes.** `PoseState::gyroBias` holds what it always held.

## Rejected alternatives
**`RelativeOrientationSensor` as the source, with the compass read once at the start.** This
removes the creep at the platform rather than in the core: Android's game rotation vector has no
magnetometer in it. It was rejected for three reasons:

- Fusing the two continuously in the core, rather than having the adapter compose the once-read
  heading onto each relative reading, needs a second attitude in `ImuSample`, which is a contract
  change.
- A resumed capture still needs the compass, and a heading read once in a bent field is wrong for
  the whole session.
- Fusing the two streams so that each covers the other's failure is the filter above with one more
  input.

[ADR 0017](0017-the-motion-port-reports-the-viewfinders-attitude.md) rejected it as a fallback
with no compass at all, because the plan is absolute. Reading the compass once answers that at the
start of a session, and the bullets above are why it is not enough.

**Gate the heading: refuse a reading whose heading disagrees by more than some angle.** This stops
a creep dead once it passes the gate, and lets it through untouched below the gate, so the
reticle's error is the gate. Worse, a session anchored in a bent field holds its wrong heading
against every true reading afterwards, because every true reading is outside the gate. A slow
correction degrades in proportion and always converges.

**Never learn the heading's offset.** This branch's first version did it, on the argument that the
compass's error is the larger one and the offset it can teach is mostly its own creep. Without an
integral the heading loop is proportional only, so a gyroscope offset about up stood ten seconds'
worth away from readings that were exactly right: 11.4° at 0.02 rad/s, never learned while the
phone kept its tilt, a sweep round the horizon included.

**Learn it from the whole disagreement.** Without the cap, a compass jump is learned as an offset
and overshot by about a tenth of the jump for most of a minute: up to 20° out from forty seconds on
after a 175° jump.

**Split by projecting the rotation vector onto up.** Also this branch's first version. After a
175° jump beside a 2° tilt it planted 0.26 rad/s of offset along up and left a still phone 149°
from a reading that never moved.
