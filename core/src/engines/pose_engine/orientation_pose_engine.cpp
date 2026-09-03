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

double Magnitude(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

}  // namespace

Result<PoseState> OrientationPoseEngine::Initial(PoseMode mode, MotionCapability capability) {
  PoseState state;
  state.mode = mode;
  state.capability = capability;
  return Ok(state);
}

Result<PoseState> OrientationPoseEngine::Integrate(const PoseState& prior,
                                                   std::span<const ImuSample> samples) {
  PoseState state = prior;

  for (const ImuSample& sample : samples) {
    if (sample.hasOrientation) {
      // Ground truth. It also clears whatever drift integration accumulated, which is the whole
      // reason to prefer it rather than blending the two.
      state.pose.orientation = Normalize(sample.orientation);
      state.absolute = true;
    } else if (state.observed && sample.timestampNs > state.pose.timestampNs) {
      const double seconds =
          static_cast<double>(sample.timestampNs - state.pose.timestampNs) * 1e-9;
      const Vec3& rate = sample.angularVelocity;
      const double magnitude = Magnitude(rate);
      if (magnitude > 1e-9) {
        // Small-angle step about the instantaneous axis. Good enough at sensor rates and
        // honestly drifty over a whole capture, which is why an absolute reading overrides it.
        state.pose.orientation = Normalize(
            Multiply(state.pose.orientation, FromAxisAngle(rate, magnitude * seconds)));
        // Dead reckoning from here on. Leaving the flag set would keep reporting an integrated
        // pose with the confidence of a measured one, and the drift would be invisible.
        state.absolute = false;
      }
    }
    state.pose.timestampNs = sample.timestampNs;
    state.observed = true;
  }

  if (!samples.empty()) state.pose.angularVelocity = samples.back().angularVelocity;
  // Zero until something has actually been observed: a caller reading confidence 0 knows the
  // orientation is a default rather than an estimate.
  state.pose.confidence = !state.observed ? 0.0 : (state.absolute ? 1.0 : 0.5);
  return Ok(state);
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

  // Measured rates when a platform supplies them, differences between attitudes when it does not.
  //
  // The distinction is not academic. The browser reports an attitude and no rates at all, so
  // every sample it produces carries a zeroed angularVelocity standing in for "not measured" —
  // and reading those zeros as a measurement reported a phone mid-swing as perfectly still.
  // Stability is what gates firing a burst, so that is the one direction this must not be wrong
  // in. `hasOrientation` is what tells the two kinds of sample apart (ADR 0015).
  double peak = 0.0;
  bool measured = false;

  for (const ImuSample& sample : samples) {
    if (!sample.hasOrientation) {
      peak = std::max(peak, Magnitude(sample.angularVelocity));
      measured = true;
    }
  }

  // Differentiating filtered attitudes is noisier than a gyroscope in exactly the band that
  // matters, so it is the fallback rather than the default.
  if (!measured) {
    const ImuSample* previous = nullptr;
    for (const ImuSample& sample : samples) {
      if (!sample.hasOrientation) continue;
      if (previous != nullptr && sample.timestampNs > previous->timestampNs) {
        const double seconds =
            static_cast<double>(sample.timestampNs - previous->timestampNs) * 1e-9;
        const double radians = AngleBetween(previous->orientation, sample.orientation);
        peak = std::max(peak, radians / seconds);
        measured = true;
      }
      previous = &sample;
    }
  }

  if (!measured) {
    // One attitude says where the phone is, not whether it is moving. Answering "perfectly still"
    // from it would let a burst fire mid-swing on the first frame after a dropout.
    return Err<double>(StatusCode::FailedPrecondition, kComponent,
                       "a single sample cannot show whether the device is moving");
  }
  return Ok(std::clamp(1.0 - peak / kUnusableRateRadPerSec, 0.0, 1.0));
}

}  // namespace sphanorama
