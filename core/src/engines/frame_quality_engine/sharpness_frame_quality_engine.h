#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "sphanorama/engines/frame_quality_engine.h"
#include "sphanorama/resource_access/frame_store_access.h"

namespace sphanorama {

// V6 — the first implementation that actually looks at the pixels.
//
// Sharpness is the variance of a Laplacian over a downscaled luma plane, which is the standard
// focus measure and is here for the reason it is standard: it responds to edges and to nothing
// else, so within a burst — same scene, same lens, and the same exposure wherever the camera can
// hold one — the frame with the most edge energy is the one the hand was steadiest for.
//
// It reads pixels, so it holds IFrameStoreAccess. That is one of the two resource accesses an
// engine may touch (docs/03 §3.3 rule 5): pixel residency is a property of the device, not of the
// algorithm, and threading a store through every Score call would invert the dependency for
// nothing.
//
// **What it does not measure yet, and why, because a zero that means "not implemented" is
// indistinguishable from a zero that means "perfect" unless it is written down.**
// `motionBlur` needs the exposure time and the focal length in pixels to turn an angular rate
// into a smear, and this engine is handed neither; a number invented from what it does have
// would rank frames by a fiction. `alignmentResidual` and `moverPenalty` are Phase 3, and both
// need registration to exist first.
class SharpnessFrameQualityEngine final : public IFrameQualityEngine {
 public:
  explicit SharpnessFrameQualityEngine(IFrameStoreAccess& frames) : frames_(frames) {}

  Result<QualityScore> Score(const FrameRef& frame, const PoseSample& pose,
                             const NodeContext& context) override;
  Result<std::vector<CandidateId>> Rank(std::span<const Candidate> candidates,
                                        const SelectionPolicy& policy) override;

 private:
  // Mean luma over the same downscale the sharpness measure walks, so a frame is read once.
  struct Measured {
    double sharpness = 0;
    double meanLuma = 0;
  };
  Result<Measured> Measure(const FrameRef& frame);

  IFrameStoreAccess& frames_;
};

}  // namespace sphanorama
