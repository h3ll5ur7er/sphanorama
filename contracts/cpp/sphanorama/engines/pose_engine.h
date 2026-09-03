#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V5 — how orientation is estimated. Absorbs sensor absence, gyro drift and the choice of fusion
// filter, so that no other component learns whether the device has usable sensors.
class IPoseEngine {
 public:
  virtual ~IPoseEngine() = default;

  // The state a session starts from. Returned rather than stored: an engine that remembered it
  // would be holding session state, which rule 4 in docs/03 §3.3 forbids (ADR 0016).
  virtual Result<PoseState> Initial(PoseMode, MotionCapability) = 0;

  // Folds a batch of samples into the prior state. Pure — the same prior and the same samples
  // give the same answer, which is what makes a fusion filter replayable against a recorded log.
  virtual Result<PoseState> Integrate(const PoseState& prior, std::span<const ImuSample>) = 0;

  // Optional visual refinement against a reference frame; also the whole of VisionOnly mode.
  virtual Result<PoseSample> Correct(const FrameRef& current, const FrameRef& reference,
                                     const PoseSample& prior) = 0;

  virtual Result<double> Stability(std::span<const ImuSample>) = 0;   // [0,1]
};

}  // namespace sphanorama
