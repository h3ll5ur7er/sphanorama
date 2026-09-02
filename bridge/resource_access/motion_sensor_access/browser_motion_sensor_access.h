#pragma once

#include "sphanorama/resource_access/motion_sensor_access.h"

namespace sphanorama::bridge {

// IMotionSensorAccess backed by the motion permission the page already negotiated.
//
// Drain is deliberately unsupported in the browser. Samples originate in JavaScript, and the
// facade already marshals them properly through the generated codec when the client calls
// OnMotion — so the browser *pushes* and the core never pulls. The native bench pulls instead,
// because it owns the recorded log it is replaying. Both are legitimate uses of one contract.
class BrowserMotionSensorAccess final : public IMotionSensorAccess {
 public:
  Result<MotionCapability> Capabilities() override;
  Status Start(int32_t requestedHz) override;
  Result<int32_t> Drain(std::span<ImuSample> out) override;
  Status Stop() override;
};

}  // namespace sphanorama::bridge
