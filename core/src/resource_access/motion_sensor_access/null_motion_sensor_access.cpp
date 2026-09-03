#include "resource_access/motion_sensor_access/null_motion_sensor_access.h"

namespace sphanorama {
namespace {
constexpr const char* kComponent = "NullMotionSensorAccess";
}

Result<MotionCapability> NullMotionSensorAccess::Capabilities() {
  return Ok(MotionCapability::None);
}
Status NullMotionSensorAccess::Start(int32_t) {
  return Fail(StatusCode::SensorUnavailable, kComponent,
              "no motion port: the browser adapter is not wired to the core yet");
}
Result<int32_t> NullMotionSensorAccess::Drain(std::span<ImuSample>) {
  return Err<int32_t>(StatusCode::FailedPrecondition, kComponent, "sensor is not running");
}
Status NullMotionSensorAccess::Stop() { return Status::Ok(); }

}  // namespace sphanorama
