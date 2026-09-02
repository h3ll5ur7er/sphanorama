#pragma once

#include "sphanorama/engines/pose_engine.h"

namespace sphanorama {

// V5, as far as it goes today.
//
// Two sources, and absolute wins. A platform that reports a fused orientation — which is what the
// browser does — has already solved the hard part, and re-deriving one from rates would add drift
// to a value that arrived correct. Where only rates are available they are integrated, which
// drifts, and an absolute reading resets that drift the moment one arrives.
//
// Visual correction is Phase 2: there is no honest minimal version of tracking features between
// frames, and a Correct() that quietly returned its input as "corrected" would make a drifting
// session look like a converging one.
class OrientationPoseEngine final : public IPoseEngine {
 public:
  Status Reset(PoseMode mode, MotionCapability capability) override;
  Result<PoseSample> Integrate(std::span<const ImuSample> samples) override;
  Result<PoseSample> Correct(const FrameRef& current, const FrameRef& reference,
                             const PoseSample& prior) override;
  Result<double> Stability(std::span<const ImuSample> samples) override;

 private:
  PoseMode mode_ = PoseMode::Fused;
  MotionCapability capability_ = MotionCapability::None;

  Quat orientation_{};
  int64_t lastTimestampNs_ = 0;
  bool observed_ = false;
  bool absolute_ = false;
};

}  // namespace sphanorama
