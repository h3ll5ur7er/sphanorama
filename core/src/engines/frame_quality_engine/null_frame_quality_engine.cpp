#include "engines/frame_quality_engine/null_frame_quality_engine.h"

namespace sphanorama {

Result<QualityScore> NullFrameQualityEngine::Score(const FrameRef&, const PoseSample&,
                                                   const NodeContext&) {
  return Ok(QualityScore{});
}

Result<std::vector<CandidateId>> NullFrameQualityEngine::Rank(
    std::span<const Candidate> candidates, const SelectionPolicy&) {
  // Insertion order. Determinism is the property that matters here: the build graph is keyed on
  // the selection, so an unstable ranking would invalidate cached stages for no reason.
  std::vector<CandidateId> ranked;
  ranked.reserve(candidates.size());
  for (const auto& candidate : candidates) ranked.push_back(candidate.id);
  return Ok(std::move(ranked));
}

}  // namespace sphanorama
