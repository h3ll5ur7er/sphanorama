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

- **Tilt** is corrected over 0.1 s, as before.
- **Heading** is corrected over 10 s, so between readings the gyroscope carries it.
- **Both are charged to the gyroscope's offset over 50 s**, each from the disagreement its own
  correction follows. For heading that is five times its correction time, which keeps its loop
  from ringing. For tilt it amends ADR 0024's 0.5 s (see the rejected alternatives): an offset
  changes over minutes, and tilt never needed it learned faster: an offset not yet learned stands
  only a correction time's worth away, a tenth of a degree at 0.02 rad/s. Gravity cannot see an
  offset about up, so on a phone that keeps its tilt only the compass can teach that part.
- **A reading after a gap of more than 0.25 s is taken whole**, as the first reading after a
  dropout is. A rate is a measurement of an instant, and held across a gap nobody sampled it says
  nothing about how the phone turned in it. The adapter already refuses a rate 200 ms from its
  attitude; a longer gap is a stall — a hidden tab, or a capture loop so far behind that the
  adapter's buffer dropped samples. The gap runs from the last sample that measured something: one
  carrying neither an attitude nor a rate does not close it.
- **A reading whose gravity is more than 30° from the prediction's is taken whole.** Gravity is the
  half of an attitude a platform measures well, so a disagreement that size is not a measurement
  error but a different frame — the page turned between portrait and landscape, which turns the
  reported frame a quarter-turn about the axis the camera looks down while the gyroscope reads
  still. Fused, the share of that turn leaning towards the world's up was corrected at the
  heading's speed, 48° out pitched thirty degrees up, and the rest was charged to the offset.

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
- a resumed capture starts on the compass, taking its first reading whole, and so do the first
  reading after a dropout and the first after a gap. None of them reaches this constant. What a
  resume needs from it is that the first session's heading stayed near the compass, so that the
  second session's cells land on the first's. This holds while the live source is
  `AbsoluteOrientationSensor`. The `deviceorientation` fallback is relative on Chrome for Android,
  so nothing there is laid out against a compass, and nothing there can creep either.

## Consequences
- **Creep.** Twenty degrees of compass creep across a two-second dwell moves the estimate 1.91°,
  against 19.99° before, at every elevation tested (0°, 60°, −45°).
- **Dropouts.** The creep teaches the offset almost nothing, so the dropout after it moves the
  estimate 0.038°, against 9.94° before.
- **Only within uninterrupted fusion.** The first reading after a dropout or a gap, and any reading
  that arrives without a measured rate, is taken as it stands, so a creep still in progress then
  arrives whole: one rate-less sample after the creep above moves the estimate from 1.91° to 20°
  out. The browser adapter drops a rate more than 200 ms from its attitude, so a late `devicemotion`
  event does this. It is deliberate: without a rate there is nothing to tell a creep from a real
  turn, and predicting no turn would make every such turn lag by ten seconds.
- **Tilt still follows in a tenth of a second.** A 5° step in pitch, or in roll at level, is
  within 0.025° after half a second. It was 0.53° before, and past the reading: an offset learned
  in half a second absorbed part of the step and overshot it.
- **Noise.** A reading 3° out on alternate samples comes out within 0.248° in tilt and 0.005° in
  heading. Tilt noise with no preferred direction, 3° rms on each horizontal axis, moves the
  heading 0.18°.
- **Tilt that goes round no longer turns the heading.** Turns about horizontal axes do not
  commute, and an offset learned in half a second followed a tilt disagreement that goes round:
  the offsets it learned, each at right angles to an up that was itself circling, added up along
  the phone's vertical. With the heading trusted slowly that reached the reticle. Measured with the
  offset learned over 0.5 s and over 50 s: a still phone whose reading's gravity circles 0.5°,
  23.5° against 0.19° of heading; a real 2° sway with every reading exact and on time, 3.25°
  against 0.057°; the same sway read 20 ms late, 12.6° against 0.25°.
- **A tilt offset is learned over minutes too.** At 0.02 rad/s about a tilt axis the estimate
  stands at most 0.11° out while it is learned, and it is learned within a quarter of a percent in
  five minutes. A dropout before then drifts at whatever is not yet learned — which in a browser,
  where every sample carries an attitude, is a dropout that does not happen.
- **A turn made during a gap arrives whole.** A phone turned 90° during 2 s with no samples is on
  the reading the moment one arrives, and nothing is charged for it.
- **A turn made during a shorter gap does not.** Under 0.25 s the gap is predicted across with the
  rate it ends on, and a turn the gyroscope did not see is corrected at the heading's speed: a
  1 rad/s sweep that stops just as a gap ends stands 2.9° out through the dwell after a 50 ms gap,
  5.7° after 0.1 s and 14° after 0.25 s. The bound is not lower because a reading taken whole
  brings the whole of a creep with it, and a phone's main thread stalls for a tenth of a second
  often enough that a lower bound would take readings whole routinely.
- **A turn of the page is taken in one sample.** A quarter-turn about the viewing axis, level or
  pitched up, or a half-turn, puts the estimate on the reading at once with nothing charged.
- **Settling.** A heading the compass holds 10° away is within 0.97° after a minute and 0.043°
  after three. After a jump of up to 175° beside a 2° tilt, gravity is right to within 0.004° a
  second later whatever the jump, and the heading settles to within 0.03° in five minutes.
- **A jump is overshot once, by about a tenth.** Learned at five times its correction time, the
  offset keeps the loop from ringing but not from passing a step: 11.6% of the jump, at its worst
  43 s later, so 20° after a 175° jump and 3.5° after a 30° one. A steady offset is learned from
  one side and not passed, at any spacing of the samples a prediction is made across.
- **An offset about up is learned slowly, and shows while it is.** At 0.005 rad/s the estimate
  peaks 2.2° out after about twenty seconds, is 1.1° out at a minute and 0.23° at two. At
  0.02 rad/s it peaks at 8.7° and is within 0.93° at two minutes. ADR 0024 learned it within
  seconds. A sweep round the horizon does not speed this up, because turning about up leaves up
  where it was in the phone's frame.
- **A roll is not purely a tilt.** At a phone pitched up, the share of a roll about the viewing axis
  that leans towards the world's up is a turn gravity cannot see. The compass corrects that share
  slowly, and the gyroscope follows it meanwhile. At the zenith the whole of a roll is heading. A
  test of a pitched roll failed while this was written for exactly this reason, which is why the
  roll case in `ATiltStepIsFollowedWithinHalfASecond` is level.
- **Heading now rests on the gyroscope's rates.** Before, an axis or unit mistake in
  `angularVelocityFromRotationRate` was corrected away within a tenth of a second. Now it would show
  as a heading that lags or overshoots a turn and then drifts back over ten seconds. The conversion
  is tested against the specification but has not met a device, and this is the first thing that
  would show if it is wrong.
- **The transient is tolerable only for a calibrated gyroscope.** On Android the gyroscope behind
  `rotationRate` is meant to be the platform's drift-compensated one, a few thousandths of a rad/s
  at most; an uncalibrated one would show the 0.02 rad/s figure above. That is not measured
  here.
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

**Never learn the heading's offset.** PR #98's first version did it, on the argument that the
compass's error is the larger one and the offset it can teach is mostly its own creep. Without an
integral the heading loop is proportional only, so a gyroscope offset about up stood ten seconds'
worth away from readings that were exactly right: 11.4° at 0.02 rad/s, never learned while the
phone kept its tilt, a sweep round the horizon included.

**Learn the offset from a capped disagreement.** PR #98's second version learned the heading's
offset from at most 0.05 rad, on the argument that a larger disagreement is the compass jumping
rather than the gyroscope drifting. It cut a jump's overshoot from 20° to 1.8°. But a clipped
input in the learning and an unclipped one in the correction settle in different places whenever
the readings are lopsided — the correction at their mean, the learning nearer their median — and an
offset makes up the difference: a still phone whose compass read 50° on one sample in ten learned
0.0072 rad/s from a gyroscope that read zero, which turns it 0.4° in every second of the next
dropout. Uncapped, the same readings teach 0.00015. Jumps are rare and are overshot by a tenth;
a compass whose errors are not symmetric is not unusual, and is the case this has to be right in.
Capping the correction too would make the two agree, but a heading mismatch would then close at no
more than 0.005 rad/s, about ten minutes for a half-turn, which is the gate's failure made slow.

**Split by projecting the rotation vector onto up.** Also PR #98's first version. After a
175° jump beside a 2° tilt it planted 0.26 rad/s of offset along up and left a still phone 149°
from a reading that never moved.

**Keep learning the tilt's offset in half a second.** ADR 0024's figure, and everything it bought
— an offset learned within seconds, for a dropout that a browser never produces — at the cost
measured above: an offset that follows a tilt disagreement going round, and a heading turned by
it. At 2 s it is still 5° of heading for the circling reading; at 5 s, 1.9°.

**Blend heading and tilt as the attitude's own parts rather than the disagreement's.** Tried while
writing PR #98's fifth version and not committed, on the theory that tilt corrections going round
were what turned the heading. They were not: with the offset still learned in half a second the
figures did not move, and with it learned over minutes the disagreement's twist and swing do as
well. It stayed out because it was a second way of saying the same thing that no test could tell
from the first.
