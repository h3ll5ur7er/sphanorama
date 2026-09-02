#pragma once

#include <deque>
#include <memory>
#include <vector>

#include "sphanorama/resource_access/motion_sensor_access.h"

namespace sphanorama {

// Replays a recorded IMU trace. Real capture sessions are driven by whatever the phone reports,
// so manager tests need a way to say "these exact samples, in this order" — otherwise a pose or
// coverage test is really a test of the device it happened to run on.
//
// Also models the outcome that is easy to forget: iOS requires a user gesture for motion access
// and the user may decline. Construct with MotionCapability::None to exercise that path.
class FakeMotionSensorAccess final : public IMotionSensorAccess {
 public:
  explicit FakeMotionSensorAccess(MotionCapability capability = MotionCapability::GyroAccel);

  Result<MotionCapability> Capabilities() override;
  Status Start(int32_t requestedHz) override;
  Result<int32_t> Drain(std::span<ImuSample> out) override;
  Status Stop() override;

  // Queue samples the session will observe. Timestamps are the caller's business.
  void Enqueue(const ImuSample& sample);
  void EnqueueSpin(int count, int64_t intervalNs, double radiansPerSecondYaw);

  int32_t RequestedHz() const { return requested_hz_; }

 private:
  MotionCapability capability_;
  std::deque<ImuSample> pending_;
  bool running_ = false;
  int32_t requested_hz_ = 0;
};

struct FakeMotionSensorAccessFactory {
  static std::unique_ptr<IMotionSensorAccess> Create() {
    auto sensor = std::make_unique<FakeMotionSensorAccess>();
    sensor->EnqueueSpin(8, 10'000'000, 0.5);
    return sensor;
  }
};

}  // namespace sphanorama
