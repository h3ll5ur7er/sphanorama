#include "engines/pose_engine/null_pose_engine.h"

namespace sphanorama {
namespace {
constexpr const char* kComponent = "NullPoseEngine";
}

Result<PoseState> NullPoseEngine::Initial(PoseMode mode, MotionCapability capability) {
  PoseState state;
  state.mode = mode;
  state.capability = capability;
  return Ok(state);
}

Result<PoseState> NullPoseEngine::Integrate(const PoseState& prior,
                                             std::span<const ImuSample> samples) {
  PoseState state = prior;
  if (!samples.empty()) {
    state.pose.timestampNs = samples.back().timestampNs;
    state.pose.angularVelocity = samples.back().angularVelocity;
  }
  state.pose.orientation = Quat{};
  // Zero confidence is the contract's way of saying "not estimated". A caller that trusts this
  // orientation is reading a value nothing produced.
  state.pose.confidence = 0.0;
  return Ok(state);
}

Result<PoseSample> NullPoseEngine::Correct(const FrameRef&, const FrameRef&,
                                            const PoseSample& prior) {
  PoseSample pose = prior;
  pose.visuallyCorrected = false;
  return Ok(pose);
}

Result<double> NullPoseEngine::Stability(std::span<const ImuSample>) {
  return Err<double>(StatusCode::Unsupported, kComponent, "no stability estimate until V5 lands");
}

}  // namespace sphanorama
