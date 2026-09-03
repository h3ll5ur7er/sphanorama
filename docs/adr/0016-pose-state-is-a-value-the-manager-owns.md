# ADR 0016 — Pose state is a value the manager owns, not a member of the engine

**Status:** accepted

## Context
Call rule 4 in [03 §3.3](../03-architecture.md) says engines never hold session state. The reason is
not tidiness: an engine whose answer depends on what it was asked previously cannot be replayed. You
cannot hand it a recorded IMU log and a starting attitude and compare two fusion filters, because
there is no way to say what "starting" means from the outside.

`IPoseEngine` broke that rule from the first commit, and nothing noticed because the only
implementation was a null object that had nothing to remember. `Reset(PoseMode, MotionCapability)`
returned `Status` and `Integrate(samples)` returned a `PoseSample` — so the accumulated attitude,
the last timestamp, and whether the last reading was absolute had nowhere to live but engine
members. The first real implementation made that concrete, and a code review on the Phase 0 pull
request caught it.

That the contract *forced* the violation is the interesting part. A rule enforced by
`tools/layer_check.py` on the include graph cannot see this: the edges were all legal.

Three ways out:

- **Leave it and write an exception.** The engine is small and the state is a quaternion. But the
  exception would apply to every future engine that fuses over time — which is most of the
  interesting ones — and Phase 1's accuracy harness is exactly the replay this would prevent.
- **Move the state into `CaptureSessionManager` as loose fields** and pass them to `Integrate` as
  arguments. Correct and unpleasant: five parameters that must be passed together, in order, and a
  new one every time the filter gets smarter.
- **Name the state.** One value type, owned by the manager, threaded through the engine.

## Decision
`PoseState` is a contract type. It carries the mode, the motion capability, the latest
`PoseSample`, and the two flags confidence is derived from (`observed`, `absolute`).

```cpp
virtual Result<PoseState> Initial(PoseMode, MotionCapability) = 0;
virtual Result<PoseState> Integrate(const PoseState& prior, std::span<const ImuSample>) = 0;
```

`Initial` replaces `Reset`: instead of putting the engine back to a known condition, it *hands back*
that condition, and the caller keeps it. `CaptureSessionManager` holds one `PoseState` per session,
which is where session state was supposed to be all along.

`Correct` and `Stability` were already pure and are unchanged.

## Consequences
- The engine is a pure function, and a test can assert it: the same prior and the same samples give
  the same answer, and the prior the caller passed is not modified. Two of the new tests do exactly
  that, and they are impossible to write against the old contract.
- A recorded session can be replayed through a candidate filter and compared against this one,
  which is what Phase 1's accuracy harness needs.
- `PoseState` crosses into TypeScript through the mirror because it lives in `types.h`. Nothing on
  the client uses it today, and it costs a generated encoder nobody calls. Splitting the contract
  types into "crosses the facade" and "crosses an engine boundary" would avoid that, and is not
  worth a second header and a second marker for one struct — revisit it if the count grows.
- Confidence became honest as a side effect. The old engine set `absolute_` when a fused reading
  arrived and never cleared it, so every later dead-reckoned pose still reported confidence 1.0.
  With the flag in a value that is copied forward per batch, the rate-integration branch clears it
  where it always should have.

## What this does not fix
`tools/layer_check.py` still cannot see a stateful engine — it reads includes, not members. This
class of violation is found by review, or by trying to write the replay test and discovering you
cannot. Worth a checker one day; not worth guessing at the shape of one now.
