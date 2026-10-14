#pragma once

#include "sphanorama/engines/pose_engine.h"

namespace sphanorama {

// V5, as far as it goes today.
//
// Two sources, each trusted for what it measures well. The gyroscope says how the phone turned; the
// platform's attitude says where it ended up. Where both arrive they are fused (ADR 0024), with the
// attitude's tilt trusted quickly, since it comes from gravity, and its heading slowly, since it
// comes from a compass that indoors creeps (ADR 0074). An attitude alone is taken as it stands, and
// rates alone are integrated, which drifts.
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
