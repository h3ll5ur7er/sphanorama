#pragma once

#include "sphanorama/engines/pose_engine.h"

namespace sphanorama {

// Null object for V5. Reports identity with zero confidence: there is no fusion filter yet, and
// a confidence of zero is how a caller learns that, rather than by trusting an orientation
// nothing produced.
class NullPoseEngine final : public IPoseEngine {
 public:
  Status Reset(PoseMode mode, MotionCapability capability) override;
  Result<PoseSample> Integrate(std::span<const ImuSample> samples) override;
  Result<PoseSample> Correct(const FrameRef& current, const FrameRef& reference,
                             const PoseSample& prior) override;
  Result<double> Stability(std::span<const ImuSample> samples) override;

 private:
  PoseMode mode_ = PoseMode::Fused;
  MotionCapability capability_ = MotionCapability::None;
};

}  // namespace sphanorama
