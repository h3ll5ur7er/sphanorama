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
// Stateless, like every engine: the carried-over estimate arrives as a PoseState and leaves as a
// new one, so the same batch of samples always folds into the same answer and a recorded session
// can be replayed through it (ADR 0016).
//
// Visual correction is Phase 2: there is no honest minimal version of tracking features between
// frames, and a Correct() that quietly returned its input as "corrected" would make a drifting
// session look like a converging one.
class OrientationPoseEngine final : public IPoseEngine {
 public:
  Result<PoseState> Initial(PoseMode mode, MotionCapability capability) override;
  Result<PoseState> Integrate(const PoseState& prior,
                              std::span<const ImuSample> samples) override;
  Result<PoseSample> Correct(const FrameRef& current, const FrameRef& reference,
                             const PoseSample& prior) override;
  Result<double> Stability(std::span<const ImuSample> samples) override;
};

}  // namespace sphanorama
