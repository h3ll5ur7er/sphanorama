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

Trusting the two at different speeds makes the filter carry more from one sample to the next — an
estimate that lags the reading on purpose, and an offset learned over minutes — and both are held
in the frame the attitude and the rates are reported in. That frame is the viewfinder's, and it
turns when the page does (ADR 0017): a filter that keeps state has to know when.

## Decision
**The disagreement between the gyroscope's prediction and the reading is split about the world's
up, and the two parts are trusted at different speeds.**

- **Tilt** is corrected over 0.1 s, as before.
- **Heading** is corrected over 10 s, so between readings the gyroscope carries it.
- **Both are charged to the gyroscope's offset over 50 s**, each from the disagreement its own
  correction follows: five times the heading's correction time, which keeps its loop from ringing.
  For tilt this amends ADR 0024's 0.5 s; the rejected alternatives say why.
- **A reading is fused only when there is a prediction worth fusing it with**: the estimate
  descends from a reading, the sample carries a measured rate, the reading is later than the last
  sample that measured anything and at most 0.25 s after it, and the reading's gravity is within
  30° of the prediction's. Otherwise it is taken whole. The first two predate this ADR (0024, 0025
  and the engine's `absolute` gate). A rate measures an instant, and held across a gap nobody
  sampled it says nothing about how the phone turned in it — a gap that long is a stall: a hidden
  tab, a capture loop so far behind that the adapter's buffer dropped samples, or the main thread
  the sensor events are delivered on. And gravity is what a platform measures well, so a reading
  whose gravity is that far out is not a measurement error: a real turn faster than the rate held
  across a gap shorter than the bound, or a reading broken outright.
- **A page turn changes the frame, not the estimate.** Turning between portrait and landscape
  steps the frame the attitude and the rates are reported in by a turn about the axis the camera
  looks down, and nothing about the phone. The adapter now reports with each sample the screen
  angle it converted both halves with (`ImuSample::screenAngleDeg`), and `PoseState` records the
  angle its estimate and offset are in. Where they differ, both are carried into the sample's
  frame before anything is compared with it, and the filter carries on. This amends
  [ADR 0017](0017-the-motion-port-reports-the-viewfinders-attitude.md), which kept the screen out
  of the contract: the conversion stays in the adapter, but whatever carries state from one sample
  to the next has to know which frame each one is in.
- **Dead reckoning across a gap breaks the descent from a reading.** Across more than 0.25 s the
  estimate no longer descends from a reading, even where a still rate integrated to nothing, so
  the next reading is taken whole.

The split is about the **world's** up, mapped into the phone's frame, not about the phone's own
vertical. Pitched up, most of a heading error lies along the axis the camera looks down, and
splitting by the phone's axes would let that part through at the fast rate. A test pitched 60°
up fails if the split is made in the phone's axes.

It is a **twist and a swing**, not a rotation vector projected onto up. The twist is the
disagreement's rotation about up; the swing is what remains, and it is exactly the disagreement
about where gravity points whatever the heading. A projection agrees for small disagreements and
not for large ones: it carries part of a small tilt along with a large heading and turns the rest
sideways, and the fast loop then charges the gyroscope for a tilt that is not there.

Ten seconds, fifty, a quarter of a second and thirty degrees are starting points on the same
footing as ADR 0024's two, not measurements. Ten seconds was chosen against both ends of the
failure: across a two-second dwell it lets through under a tenth of a creep, and a compass that
jumps while readings keep arriving, or a gyroscope offset about up, is corrected at this speed, so
slower leaves either standing longer. What a resumed capture needs from it is that the first
session's heading stayed near the compass, so that the second session's cells — laid out from a
first reading taken whole — land on the first's. That holds while the live source is
`AbsoluteOrientationSensor`; the `deviceorientation` fallback is relative on Chrome for Android, so
nothing there is laid out against a compass, and nothing there can creep either.

## Consequences
- **Creep.** Twenty degrees of compass creep across a two-second dwell moves the estimate 1.91°,
  against 19.99° before, at every elevation tested (0°, 60°, −45°).
- **Dropouts.** The creep teaches the offset almost nothing, so the dropout after it moves the
  estimate 0.038°, against 9.94° before.
- **A reading taken whole brings a creep with it.** Whatever a creep had reached arrives at once:
  one rate-less sample after the creep above moves the estimate from 1.91° to 20° out. The browser
  adapter drops a rate more than 200 ms from its attitude, so a late `devicemotion` event does
  this, and so does a gap during a creep. It is deliberate: without a prediction
  worth trusting there is nothing to tell a creep from a real turn, and predicting no turn would
  make every such turn lag by ten seconds. What taking whole buys: a phone turned 90° during a
  2 s gap, or turned a quarter-turn about the camera's axis without the gyroscope seeing it, level
  or pitched 30° up, or a half-turn, is on the reading in one sample with nothing charged. Fused
  under the split, the pitched quarter-turn stood 48° out a second later and taught the offset
  0.026 rad/s. With the split but before the rule on
  dead reckoning (PR #98's sixth version), a rate-only sample reading still after a 2 s gap left
  the next reading 89.9° out at confidence 1; it is now taken whole too.
- **A page turn changes nothing but the frame.** The same session reported with and without a
  page turn part-way through comes out the same, one answer re-expressed in the other's frame, to
  the precision of the arithmetic: level, pitched either way, at the zenith, after a stall, ahead
  of the rates, and in the middle of a creep. A creep in progress is not taken whole by it. And
  `Stability`, which differentiates attitudes where no rate was measured, compares them in one
  frame: across a page turn a still phone's attitudes are a quarter-turn apart, and it scored 0.
- **The adapter's angle is believed.** A port that reports no screen leaves it at zero and turns
  nothing; one whose angle is not a number keeps the frame it had. One that reports a wrong angle
  turns the offset wrong, and nothing in the readings would say so. Below about 68.5° of pitch the
  gravity rule puts the estimate back on the reading; past it the wrong turn is mostly heading, and
  is corrected at the heading's speed — 78° to 81° out a second later.
- **A turn made during a shorter gap is not taken whole.** Under 0.25 s the gap is predicted across
  with the rate it ends on, and a turn the gyroscope did not see is corrected at the heading's
  speed: a 1 rad/s sweep that stops just as a gap ends stands 2.9° out through the dwell after a
  50 ms gap, 5.7° after 0.1 s and 14° after 0.25 s. The bound is not lower because a reading taken
  whole brings a creep with it, and a phone's main thread is expected to stall for a tenth of a
  second often enough that a lower bound would take readings whole routinely; that is not measured
  here.
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
- **A tilt offset is learned over minutes too.** An offset not yet learned stands only a
  correction time's worth away: at 0.02 rad/s about a tilt axis, at most 0.11°, and it is learned
  within a quarter of a percent in five minutes. A dropout before then drifts at whatever is not
  yet learned — which in a browser, where every sample carries an attitude, is a dropout that does
  not happen.
- **Settling.** A heading the compass holds 10° away is within 0.97° after a minute and 0.043°
  after three. After a jump of up to 175° beside a 2° tilt, gravity is right to within 0.004° a
  second later whatever the jump, and the heading settles to within 0.03° in five minutes.
- **A jump is overshot once, by about a tenth.** Learned at five times its correction time, the
  offset keeps the loop from ringing but not from passing a step: 11.6% of the jump, at its worst
  43 s later, so 20° after a 175° jump and 3.5° after a 30° one. A steady offset is learned from
  one side and not passed, at any spacing of the samples a prediction is made across.
- **An offset about up is learned slowly, and shows while it is.** The heading carries an unlearned
  offset for its ten seconds: at 0.005 rad/s the estimate peaks 2.2° out after about twenty
  seconds, is 1.1° out at a minute and 0.23° at two. At 0.02 rad/s it peaks at 8.7° and is within
  0.93° at two minutes. ADR 0024 learned it within seconds. A sweep round the horizon does not
  speed this up, because turning about up leaves up where it was in the phone's frame.
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
- **The clock is moved only by a sample that measured something.** One carrying neither an attitude
  nor a rate does not close a gap, and does not start the stream's clock. `PoseState::observed`'s
  comment in the contract says so.
- **The contract changes.** `ImuSample` and `PoseState` each carry a screen angle, the browser
  port's flat sample grows from seventeen doubles to eighteen, and `PoseState::gyroBias` says which
  frame it is in. The page and the core are fetched and cached separately, so the first launch
  after a deploy can pair a page with the core before it; read at the other's stride, every sample
  decoded as a plausible attitude and aimed 73.5° out at worst. So each side now names its stride
  to the other and reads nothing from one that names another or none, which is every page and core
  from before this one: such a launch has no motion until the next load, rather than wrong motion.

## Rejected alternatives
**Leave the page turn to the readings.** Three versions of PR #98 did, and each missed a case the
next one found:

- **Not at all.** Taken whole as a reading 30° out, the turn left the learned offset in the old
  frame, pointing a quarter-turn wrong: 8.7° out at 0.02 rad/s while it was learned again.
- **Where gravity was 30° out**, turning the offset by the disagreement. That missed a turn that
  arrived after a stall or without a rate (8.7° again), and every page turn past 68.5° of pitch,
  which gravity barely sees: fused, 78° out a second later. It also turned the offset for a real
  turn the gyroscope missed inside a short gap, which does not change the frame: 1.6° to 8.7°.
- **By its shape**, within 10° of a quarter- or half-turn about the camera's axis. That missed every
  page turn made while the estimate lagged a creeping compass by more than the tolerance — the creep
  this ADR exists for — and took a real roll of that size, or a compass jump of that size near a
  pole, for a page turn.

The readings cannot say which frame they are in; the adapter that converted them can.

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
