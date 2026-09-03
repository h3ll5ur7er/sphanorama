# ADR 0015 — The IMU sample carries an absolute orientation, and the browser fills it

**Status:** accepted

## Context
`MotionCapability::OrientationOnly` has been in the contract since the first pass. It describes a
device that reports a fused attitude rather than raw rates — and `ImuSample` had nowhere to put
one. It carried `angularVelocity`, `acceleration` and an optional `magneticField`, all of which a
platform reporting only an attitude has to invent.

That platform is the one we ship on. `DeviceOrientationEvent` gives alpha/beta/gamma — an
orientation. `DeviceMotionEvent.rotationRate` exists, but it is unavailable on desktop Safari, gated
behind the same iOS permission, and reported in degrees per second with a sign convention that
differs between engines. The fused attitude is the reliable reading on every browser we target, and
the contract had no field for it.

Two ways to fix that:

- **Differentiate the attitude into a rate in the adapter**, so `ImuSample` stays a pure rate
  carrier. It fabricates a measurement: the difference of two fused, filtered estimates over a
  jittery event interval is noisy in exactly the band a pose filter cares about, and the pose
  engine would then integrate it straight back into the attitude it started from.
- **Widen `ImuSample`** with an optional absolute orientation, and let the pose engine prefer it.

## Decision
`ImuSample` gains `hasOrientation` and `orientation`. The flag is not decoration: a zero quaternion
and an unset one are indistinguishable, and a pose engine that could not tell them apart would
happily integrate an uninitialised sensor read.

`OrientationPoseEngine` prefers an absolute orientation when one is present and integrates rates
when one is not, so a platform that reports rates and a platform that reports an attitude both work
without any component above the engine knowing which it is. Confidence reports which happened —
`1.0` for an observed attitude, `0.5` for an integrated one, `0` for a batch that carried neither.

The **frame conversion happens in the browser adapter**, not in the engine. The browser reports
intrinsic Z-X'-Y'' angles against an east-north-up earth frame; the core plans in a Y-up frame whose
-Z is forward, which is what `FromAzimuthElevation` and `Direction` are written against.
`shell/src/access/orientation.ts` composes the Euler triple and rotates the result into the core's
frame. Which angles a platform reports is V10 — *where motion data comes from* — so a second
platform reporting a rotation matrix, a Generic Sensor `AbsoluteOrientationSensor` quaternion or a
replayed log changes that file and nothing above it. How those readings become an attitude stays
V5's, in `PoseEngine`.

## Consequences
- The sample is bigger, by a flag and four doubles. At 60 Hz with a 32-sample drain that is under
  10 kB/s across the boundary, against a budget dominated by frames.
- `MotionCapability::OrientationOnly` now describes something the contract can carry. It did not
  before, which made it a promise the wire could not keep.
- A platform that reports both an attitude and rates sends both, and the engine's preference — not
  the adapter's — decides which is used. That decision is V2's and belongs in the engine.
- The zeroed rate fields on a browser sample are not measurements. `hasOrientation` is what
  distinguishes them from a genuinely motionless device, and any consumer that reads
  `angularVelocity` without checking it will read a device at rest.
