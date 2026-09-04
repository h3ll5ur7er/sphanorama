# ADR 0024 — The pose engine fuses attitude with rates, and the gyroscope's offset is session state

**Status:** accepted

## Context
`OrientationPoseEngine` has had one rule since it was written: if a sample carries an absolute
orientation, take it; otherwise integrate the rate. [ADR 0015](0015-absolute-orientation-in-the-imu-sample.md)
established the sample shape that makes the distinction possible, and preferring the fused attitude
was right — a browser's attitude is already sensor-fused by the platform, and integration drifts.

The rule has a hole that is invisible today and stops being invisible the moment any platform
reports both. A sample carrying an attitude *and* a rate takes the first branch, and the rate is
discarded — so an engine handed the two signals whose weaknesses cancel would use only one of
them. The fix and the failure are the same piece of code, which is why this lands before the
adapter that produces such a sample rather than after it.

What each signal is bad at is the whole argument. An absolute attitude is a drift-bounded
reference for where the device points and says nothing about how fast it is turning; indoors it is
also a magnetometer reading that wanders by degrees between samples. A gyroscope is precise about
change and knows nothing about absolute direction, and it does not read zero at rest — that offset
integrates straight into the estimate during every second when no attitude is arriving to correct
it.

Neither is ground truth and this ADR does not make one: `01 §1.3` says the sensor pose is a prior
and never truth, and fusing two priors produces a better prior. What the attitude bounds is drift,
which is the one thing a gyroscope cannot bound for itself.

## Decision
**A complementary filter, in the form that estimates the gyroscope's offset as it runs.** On a
sample carrying both, the engine predicts forward with the bias-corrected rate, measures the
disagreement between that prediction and the reading, applies part of it to the estimate and
charges the rest to the offset.

Three properties of the shape are worth stating because each is a decision:

- **The gains are times, not per-sample fractions.** A filter tuned as "this share of the error
  every sample" smooths four times harder on a 200 Hz native stream than on a 60 Hz browser one —
  the sensor's rate becomes a tuning parameter nobody chose. Expressed as time constants, the
  elapsed time in each sample does the work. Correction settles over 0.1 s, the offset over 0.5 s;
  both are starting points on the same footing as `kUnusableRateRadPerSec`, to be tuned when there
  are real captures to tune against.
- **No stillness detector.** The classic way to learn a gyroscope's offset is to average its
  output while the device is judged still, which needs a judgement that can be wrong. Here the
  offset is whatever part of the disagreement keeps pointing the same way: noise does not, and
  cancels; a device that is genuinely turning produces a prediction the reading agrees with, so
  there is nothing to charge.
- **The correction always takes the short way round.** A quaternion and its negation are the same
  rotation, so a 160° disagreement can come out of the arithmetic as 200° the other way. Taken
  literally that moves the estimate *away* from the reading and charges the offset an error of the
  wrong sign.

**`PoseState` gains `Vec3 gyroBias`, which is a contract change.** It is session state and an
engine is stateless per session ([ADR 0016](0016-pose-state-is-a-value-the-manager-owns.md)); an
offset kept inside the engine would be shared by every session in the process and would outlive
the device being put down and picked up. It goes where the mode, the capability and the `absolute`
flag already are, and the manager threads it back in exactly as it does those.

**Fusion is switched on by the capability reporting a gyroscope, not by inspecting the numbers.**
That is `GyroAccel` or `GyroAccelMag` — the latter is the former with a magnetometer on top, so
testing for one exact value would have handed the better-equipped device the worse behaviour, and
the device whose attitude most needs help the least of it. On an
`OrientationOnly` stream every sample carries a zeroed `angularVelocity` standing in for "not
measured", and reading those zeros as a measurement of stillness is the mistake `Stability` had to
be fixed for once already. Deciding from the capability also means every platform shipping today
behaves exactly as before: with no gyroscope there is nothing to fuse, and blending would add lag
to the one signal there is.

## Consequences
- The estimate survives an attitude dropout. A 0.02 rad/s offset — a bit over a degree a second,
  and unremarkable — put 1.15° of yaw into a second of dead reckoning from a device that never
  moved; with the offset learned it is under 0.2°.
- A jumpy indoor magnetometer stops reaching the reticle. A reading 3° out on every sample comes
  out under 1.5°, which is the difference between a reticle that shivers on a target it is holding
  and one that sits on it.
- **Nothing runs it yet, and that is the next piece of work rather than an oversight.** The browser
  adapter reports `OrientationOnly` deliberately — it listens for a fused attitude and never
  adapts `DeviceMotionEvent.rotationRate` — so no sample carrying both exists outside the tests.
  Claiming `GyroAccel` before those rates are real would be worse than not claiming it: the engine
  would fuse against zeros.
- **`Stability` is left alone and is now the weaker half.** It tells "no rate measured" from "rate
  measured as zero" by `hasOrientation`, which is exactly the heuristic a fused sample breaks —
  and it takes only a span of samples, so it has no capability to consult. On a fused stream it
  falls back to differentiating attitudes, which is correct and noisier rather than wrong. Fixing
  it properly means passing the state into `Stability`, which is a second contract change and
  belongs to whoever needs the accuracy.

## Rejected alternative
**Keep preferring the attitude, and use rates only when one is absent.** This is today's rule, it
is one branch, and on an already-fused platform attitude it is very nearly right: the OS has done
the fusion, and a second filter on top can add lag without adding accuracy.

It was rejected because the case it is right for is the case where the platform hands over a
finished answer, and that is not the case this project is aimed at. `deviceorientation` on a
mid-range Android indoors is a magnetometer reading with a filter on it, arriving at whatever rate
the browser feels like; the gyroscope underneath it is the thing that knows the phone just moved.
Preferring the attitude also means the offset can never be observed at all — there is no
disagreement to learn from — so every dropout starts from zero knowledge, permanently.

The lag objection is real and is what the correction time constant is for. If a platform's
attitude turns out to be good enough that fusing it is a net loss, the honest response is to
report `OrientationOnly` for that platform, which switches this off by the same rule that
switches it on.
