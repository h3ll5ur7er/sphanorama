# ADR 0025 — A sample says whether its rate was measured, and the capability stops deciding

**Status:** accepted
**Refines:** [ADR 0024](0024-orientation-and-rates-are-fused-and-the-bias-is-state.md)

## Context
[ADR 0024](0024-orientation-and-rates-are-fused-and-the-bias-is-state.md) made the pose engine fuse
an attitude with gyroscope rates, and switched that on with `MotionCapability::GyroAccel`. The
reasoning was sound at the time: on a stream with no gyroscope every sample carries a zeroed
`angularVelocity` standing in for "not measured", and reading those zeros as a measurement of
stillness is the mistake `Stability` had already been fixed for once.

It stopped being sound the moment the rates became real. Three things broke it at once:

- **`Capabilities()` is asked before `Start()`.** `CaptureSessionManager::Begin` reads the
  capability at line 98 and starts the sensor at line 121, so an adapter that only learns whether
  rates arrive *by receiving one* can never answer in time. Answering conservatively means
  answering `OrientationOnly` forever.
- **A platform can have the API and report nothing.** Desktop Chrome fires `devicemotion` with a
  null `rotationRate`. A capability derived from the constructor existing is a claim the samples
  then contradict.
- **`Stability` cannot consult a capability at all.** It takes a span of samples and nothing else,
  and told "no rate measured" from "rate measured as zero" by `hasOrientation` — which is exactly
  the heuristic a sample carrying both breaks. ADR 0024 named this as needing a second contract
  change; this is it, and it is smaller than the one that ADR imagined.

## Decision
**`ImuSample` gains `bool hasAngularVelocity`, and it is what decides.** The engine fuses when a
sample carries an attitude *and* says its rate was measured; `Stability` reads rates from samples
that say so, rather than from samples that lack an attitude. `MotionCapability` goes back to
describing the platform, and nothing reads it to decide whether a number is real.

It is the same distinction `hasOrientation` already draws, for the same reason, and the symmetry is
the argument: zero is a real rate exactly as zero is a real angle, so neither field can say on its
own whether the device was still or nothing looked.

**The browser adapter listens for `DeviceMotionEvent.rotationRate` and attaches it to the attitude
samples that follow.** Four decisions inside that are worth recording:

- **The attitude decides when a sample exists**, and the most recent rate rides along with it.
  Fusion wants a prediction and a reading describing the same instant; a rate on a sample of its
  own would be integrated on one tick and corrected on the next, which is the lag fusion exists
  to remove.
- **A rate more than 200 ms from the attitude is not attached, in either direction.** Two streams
  arrive independently, so the rate is always a little out; past a fifth of a second it describes
  a different moment, and the engine would correct against it with no way to tell. The bound was
  one-sided at first, on the reasoning that a rate *newer* than the attitude is the two streams
  interleaving — true of a few milliseconds of skew between events on the same clock, and no
  reason to accept any amount of it.
- **A partial `rotationRate` is dropped whole**, the same rule the orientation angles already
  follow: only null means unavailable, so a missing axis is not completed with a zero.
- **The motion grant is separate on iOS and its denial is not a failed start.** A user who granted
  orientation and denied motion gets the session that existed before rates did, rather than no
  session.

**The rate is converted into the viewfinder's frame, inverting the screen rotation.** The attitude
becomes the viewfinder's by post-multiplying the screen rotation; a rate is a body-frame vector, so
post-multiplying by `s` re-expresses it through `s⁻¹`. Getting this wrong is invisible in portrait
and wrong by 90° in landscape, which is how the app is actually held. `EARTH_TO_CORE` does not
appear: it is a change of *world* frame and a body-frame rate does not see it, which is what lets
the engine post-multiply the rate straight onto an attitude.

**The motion wire format gains a double, and loses a duplicated constant.** The flat layout between
`capture-host.ts` and `browser_motion_sensor_access.cpp` is 17 doubles now, with each optional group
preceded by the flag that says whether it was measured. The stride was written in three places —
both ends and, silently, inside an `EM_JS` body as `flat.length / 16` — and the third is now passed
in as an argument, because a decoder reading the wrong number of samples produces plausible ones.

## Consequences
- Fusion runs on a real device for the first time, on any browser that reports both.
- **`Stability` is fixed rather than left as the weaker half.** A phone swung between two attitudes
  that happen to match came back `1.0` — perfectly still — with the gyroscope in the same sample
  reading 3 rad/s. There is a test for it. Stability gates firing a burst, so still-when-swinging
  is the one direction it must never be wrong in.
- A capability that over-claims costs nothing, which is why `GyroAccel` can now be reported from
  the API's presence. Nothing downstream trusts it about an individual number.
- **A test was deleted rather than updated**: `reports OrientationOnly even where the motion API
  exists` asserted the behaviour this reverses, and its reasoning — that claiming `GyroAccel` would
  have the engine fusing against zeros — is exactly what the flag makes impossible.
- One thing does not improve: **still no device has run any of this.** The conversion is checked
  against the attitudes it should agree with, on every axis and at two screen angles, which is the
  strongest check available without hardware.

## Rejected alternative
**Have the adapter report `GyroAccel` only after seeing a rate, and keep the capability
authoritative.** It needs no contract change, and it is honest in the sense that matters: the
adapter would never claim rates it had not seen.

It cannot work here, and the reason is worth writing down because it looks like it should.
`Capabilities()` is read before `Start()`, so at the moment the question is asked no sample has
arrived and the honest answer is always `OrientationOnly` — fusion would be permanently off, and
the failure would look exactly like the fusion not working. Making it work would mean reordering
the manager's `Begin` around a browser detail, which is the dependency the resource-access layer
exists to prevent.

The deeper objection is that a capability is the wrong shape for this. It describes a platform for
a whole session; whether a particular reading happened is a fact about a particular sample. Two
facts, two homes.
