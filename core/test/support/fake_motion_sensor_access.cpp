#include "support/fake_motion_sensor_access.h"

#include <algorithm>

namespace sphanorama {
namespace {
constexpr const char* kComponent = "FakeMotionSensorAccess";
}

FakeMotionSensorAccess::FakeMotionSensorAccess(MotionCapability capability)
    : capability_(capability) {}

Result<MotionCapability> FakeMotionSensorAccess::Capabilities() { return Ok(capability_); }

Status FakeMotionSensorAccess::Start(int32_t requestedHz) {
  if (capability_ == MotionCapability::None) {
    return Fail(StatusCode::SensorPermissionDenied, kComponent,
                "motion access unavailable or declined");
  }
  if (requestedHz <= 0) {
    return Fail(StatusCode::InvalidArgument, kComponent, "sample rate must be positive");
  }
  requested_hz_ = requestedHz;
  running_ = true;
  return Status::Ok();
}

Result<int32_t> FakeMotionSensorAccess::Drain(std::span<ImuSample> out) {
  if (!running_) {
    return Err<int32_t>(StatusCode::FailedPrecondition, kComponent, "sensor is not running");
  }
  const auto count = static_cast<int32_t>(
      std::min(out.size(), pending_.size()));
  for (int32_t i = 0; i < count; ++i) {
    out[static_cast<size_t>(i)] = pending_.front();
    pending_.pop_front();
  }
  return Ok(count);
}

Status FakeMotionSensorAccess::Stop() {
  running_ = false;
  return Status::Ok();
}

void FakeMotionSensorAccess::Enqueue(const ImuSample& sample) { pending_.push_back(sample); }

void FakeMotionSensorAccess::EnqueueSpin(int count, int64_t intervalNs,
                                         double radiansPerSecondYaw) {
  for (int i = 0; i < count; ++i) {
    ImuSample sample;
    sample.timestampNs = static_cast<int64_t>(i) * intervalNs;
    sample.angularVelocity = Vec3{0.0, radiansPerSecondYaw, 0.0};
    sample.hasAngularVelocity = true;
    sample.acceleration = Vec3{0.0, 0.0, 9.81};
    pending_.push_back(sample);
  }
}

}  // namespace sphanorama
