#include "resource_access/motion_sensor_access/browser_motion_sensor_access.h"

#include <emscripten/emscripten.h>

namespace sphanorama::bridge {
namespace {

constexpr const char* kComponent = "BrowserMotionSensorAccess";

// Returned as an index into MotionCapability rather than a string: the enum's order is the
// contract, and the generated codec encodes it the same way.
EM_JS(int32_t, host_motion_capability, (), {
  if (!Module.sphHost) return 0;
  const order = ['None', 'OrientationOnly', 'GyroAccel', 'GyroAccelMag'];
  const index = order.indexOf(Module.sphHost.motionCapability());
  return index < 0 ? 0 : index;
});

}  // namespace

Result<MotionCapability> BrowserMotionSensorAccess::Capabilities() {
  return Ok(static_cast<MotionCapability>(host_motion_capability()));
}

Status BrowserMotionSensorAccess::Start(int32_t requestedHz) {
  if (requestedHz <= 0) {
    return Fail(StatusCode::InvalidArgument, kComponent, "sample rate must be positive");
  }
  // The page started the sensor before it began a session; there is nothing to start here.
  return host_motion_capability() == 0
             ? Fail(StatusCode::SensorUnavailable, kComponent,
                    "the page reports no motion sensors")
             : Status::Ok();
}

Result<int32_t> BrowserMotionSensorAccess::Drain(std::span<ImuSample>) {
  return Err<int32_t>(StatusCode::Unsupported, kComponent,
                      "the browser pushes samples through OnMotion; it does not pull");
}

Status BrowserMotionSensorAccess::Stop() { return Status::Ok(); }

}  // namespace sphanorama::bridge
