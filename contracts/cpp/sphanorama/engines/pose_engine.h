#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V5 — how orientation is estimated: gyro drift, the choice of fusion filter, and which of the
// platform's readings are worth trusting, so that no other component learns how an attitude was
// arrived at.
//
// Sensor *absence* is no longer among them. This engine absorbed it until ADR 0044, switching to
// `PoseMode::VisionOnly` so nothing above needed to know; the mode is still here and nothing
// selects it, because a session that has no motion sensor is refused rather than served
// (`ICaptureSessionManager::Begin`). It is what a vision-only capture would run in the day
// `IRegistrationEngine` can place a live stream of frames without an external reference.
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
