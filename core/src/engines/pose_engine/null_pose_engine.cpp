#include "engines/pose_engine/null_pose_engine.h"

namespace sphanorama {
namespace {
constexpr const char* kComponent = "NullPoseEngine";
}

Status NullPoseEngine::Reset(PoseMode mode, MotionCapability capability) {
  mode_ = mode;
  capability_ = capability;
  return Status::Ok();
}

Result<PoseSample> NullPoseEngine::Integrate(std::span<const ImuSample> samples) {
  PoseSample pose;
  if (!samples.empty()) {
    pose.timestampNs = samples.back().timestampNs;
    pose.angularVelocity = samples.back().angularVelocity;
  }
  pose.orientation = Quat{};
  // Zero confidence is the contract's way of saying "not estimated". A caller that trusts this
  // orientation is reading a value nothing produced.
  pose.confidence = 0.0;
  return Ok(pose);
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
