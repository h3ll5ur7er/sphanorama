#pragma once

#include "sphanorama/engines/frame_quality_engine.h"

namespace sphanorama {

// Null object for V6. Scores everything zero and ranks by insertion order — deterministic, which
// is the property the build graph actually depends on, and transparently not a quality judgement.
class NullFrameQualityEngine final : public IFrameQualityEngine {
 public:
  Result<QualityScore> Score(const FrameRef& frame, const PoseSample& pose,
                             const NodeContext& context) override;
  Result<std::vector<CandidateId>> Rank(std::span<const Candidate> candidates,
                                        const SelectionPolicy& policy) override;
};

}  // namespace sphanorama
