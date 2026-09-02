#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V10 — where motion data comes from. Reporting MotionCapability::None is a normal outcome, not
// an error: iOS requires a user gesture and the user may decline.
class IMotionSensorAccess {
 public:
  virtual ~IMotionSensorAccess() = default;

  virtual Result<MotionCapability> Capabilities() = 0;
  virtual Status Start(int32_t requestedHz) = 0;

  // Copies out of the shared ring buffer; returns how many samples were written.
  virtual Result<int32_t> Drain(std::span<ImuSample> out) = 0;

  virtual Status Stop() = 0;
};

}  // namespace sphanorama
