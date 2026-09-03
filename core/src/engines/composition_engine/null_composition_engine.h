#pragma once

#include "sphanorama/engines/composition_engine.h"

namespace sphanorama {

// Null object for V8: every call returns Unsupported, for the same reason as registration.
class NullCompositionEngine final : public ICompositionEngine {
 public:
  Result<GainMap> CompensateExposure(const GlobalSolution& solution,
                                     std::span<const FrameRef> frames) override;
  Result<GhostReport> DetectGhosts(const GlobalSolution& solution,
                                   std::span<const Candidate> allCandidates) override;
  Result<SeamMap> FindSeams(const GlobalSolution& solution, const GainMap& gains,
                            const GhostReport& ghosts, const BuildSpec& spec) override;
  Result<FrameRef> BlendTile(const GlobalSolution& solution, const GainMap& gains,
                             const SeamMap& seams, const BuildSpec& spec,
                             int32_t tileX, int32_t tileY) override;
  Result<FrameRef> RenderPreview(const GlobalSolution& solution, const GainMap& gains,
                                 int32_t maxWidth) override;
};

}  // namespace sphanorama
