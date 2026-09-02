#pragma once

#include "sphanorama/resource_access/motion_sensor_access.h"

namespace sphanorama {

// Stands in until the browser port lands. Reports MotionCapability::None, which is a supported
// configuration rather than an error — the same answer an iPhone gives when motion access is
// declined (docs/03 UC-4).
class NullMotionSensorAccess final : public IMotionSensorAccess {
 public:
  Result<MotionCapability> Capabilities() override;
  Status Start(int32_t requestedHz) override;
  Result<int32_t> Drain(std::span<ImuSample> out) override;
  Status Stop() override;
};

}  // namespace sphanorama
