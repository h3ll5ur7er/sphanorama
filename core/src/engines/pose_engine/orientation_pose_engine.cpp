#include "engines/pose_engine/orientation_pose_engine.h"

#include <algorithm>
#include <cmath>

#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr const char* kComponent = "OrientationPoseEngine";

// Above this the device is being swung rather than aimed, and a burst fired here yields five
// blurred frames and no good one. Chosen as a starting point, not a measurement — it is a config
// key's worth of tuning once real captures exist.
constexpr double kUnusableRateRadPerSec = 2.0;

}  // namespace

Status OrientationPoseEngine::Reset(PoseMode mode, MotionCapability capability) {
  mode_ = mode;
  capability_ = capability;
  orientation_ = Quat{};
  lastTimestampNs_ = 0;
  observed_ = false;
  absolute_ = false;
  return Status::Ok();
}

Result<PoseSample> OrientationPoseEngine::Integrate(std::span<const ImuSample> samples) {
  for (const ImuSample& sample : samples) {
    if (sample.hasOrientation) {
      // Ground truth. It also clears whatever drift integration accumulated, which is the whole
      // reason to prefer it rather than blending the two.
      orientation_ = Normalize(sample.orientation);
      absolute_ = true;
    } else if (observed_ && sample.timestampNs > lastTimestampNs_) {
      const double seconds = static_cast<double>(sample.timestampNs - lastTimestampNs_) * 1e-9;
      const Vec3& rate = sample.angularVelocity;
      const double magnitude =
          std::sqrt(rate.x * rate.x + rate.y * rate.y + rate.z * rate.z);
      if (magnitude > 1e-9) {
        // Small-angle step about the instantaneous axis. Good enough at sensor rates and
        // honestly drifty over a whole capture, which is why an absolute reading overrides it.
        orientation_ = Normalize(Multiply(orientation_, FromAxisAngle(rate, magnitude * seconds)));
      }
    }
    lastTimestampNs_ = sample.timestampNs;
    observed_ = true;
  }

  PoseSample pose;
  pose.timestampNs = lastTimestampNs_;
  pose.orientation = orientation_;
  if (!samples.empty()) pose.angularVelocity = samples.back().angularVelocity;
  // Zero until something has actually been observed: a caller reading confidence 0 knows the
  // orientation is a default rather than an estimate.
  pose.confidence = !observed_ ? 0.0 : (absolute_ ? 1.0 : 0.5);
  return Ok(pose);
}

Result<PoseSample> OrientationPoseEngine::Correct(const FrameRef&, const FrameRef&,
                                                   const PoseSample& prior) {
  PoseSample pose = prior;
  pose.visuallyCorrected = false;
  return Ok(pose);
}

Result<double> OrientationPoseEngine::Stability(std::span<const ImuSample> samples) {
  if (samples.empty()) {
    // Not "perfectly still": reporting stability for a dropout would let a burst fire blind.
    return Err<double>(StatusCode::FailedPrecondition, kComponent,
                       "no samples to judge stability from");
  }

  double peak = 0.0;
  for (const ImuSample& sample : samples) {
    const Vec3& rate = sample.angularVelocity;
    peak = std::max(peak, std::sqrt(rate.x * rate.x + rate.y * rate.y + rate.z * rate.z));
  }
  return Ok(std::clamp(1.0 - peak / kUnusableRateRadPerSec, 0.0, 1.0));
}

}  // namespace sphanorama
