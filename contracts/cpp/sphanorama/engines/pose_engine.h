#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V5 — how orientation is estimated. Absorbs sensor absence, gyro drift and the choice of fusion
// filter, so that no other component learns whether the device has usable sensors.
class IPoseEngine {
 public:
  virtual ~IPoseEngine() = default;

  virtual Status Reset(PoseMode, MotionCapability) = 0;
  virtual Result<PoseSample> Integrate(std::span<const ImuSample>) = 0;

  // Optional visual refinement against a reference frame; also the whole of VisionOnly mode.
  virtual Result<PoseSample> Correct(const FrameRef& current, const FrameRef& reference,
                                     const PoseSample& prior) = 0;

  virtual Result<double> Stability(std::span<const ImuSample>) = 0;   // [0,1]
};

}  // namespace sphanorama
