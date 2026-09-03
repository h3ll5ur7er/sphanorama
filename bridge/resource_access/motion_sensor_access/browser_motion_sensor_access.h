#pragma once

#include "sphanorama/resource_access/motion_sensor_access.h"

namespace sphanorama::bridge {

// IMotionSensorAccess backed by the motion permission the page already negotiated.
//
// Samples originate in JavaScript, so the page buffers them as its orientation listener fires
// and this drains that buffer — the same resident-host pattern as the project store (ADR 0014).
//
// An earlier version refused Drain outright on the grounds that the client pushes samples through
// the facade instead. That made the port unsubstitutable for its own contract: the shared suite
// requires Drain to work after Start, so the browser implementation would have passed every test
// against the fake and failed only on a phone. Pushing is still supported, and the manager pulls
// only when a client passed nothing.
class BrowserMotionSensorAccess final : public IMotionSensorAccess {
 public:
  Result<MotionCapability> Capabilities() override;
  Status Start(int32_t requestedHz) override;
  // Drains the buffer the page fills as orientation events arrive — the same resident-host
  // pattern as the project store (ADR 0014), so this port satisfies its contract rather than
  // refusing the one call the contract is built around.
  Result<int32_t> Drain(std::span<ImuSample> out) override;
  Status Stop() override;

 private:
  // The contract's precondition, which this port has to enforce itself: Drain is refused before
  // Start and after Stop. The page's listener is what fills the buffer, but the *session* is what
  // this flag tracks, and a manager relying on the refusal would otherwise get an empty success
  // from the browser and a FailedPrecondition from every other implementation.
  bool running_ = false;
};

}  // namespace sphanorama::bridge
