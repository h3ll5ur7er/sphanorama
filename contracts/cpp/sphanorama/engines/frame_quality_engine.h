#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V6 — what makes a frame the best of its burst. Expected to be the most-tuned component in the
// system, which is why selection policy is a parameter rather than a compiled-in weighting.
class IFrameQualityEngine {
 public:
  virtual ~IFrameQualityEngine() = default;

  virtual Result<QualityScore> Score(const FrameRef&, const PoseSample&, const NodeContext&) = 0;

  // Ranked best-first. The manager decides what to do with the ranking; ties break
  // deterministically so that the same candidates always produce the same build.
  virtual Result<std::vector<CandidateId>> Rank(std::span<const Candidate>,
                                                const SelectionPolicy&) = 0;
};

}  // namespace sphanorama
