#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V8 — how pixels become one image.
class ICompositionEngine {
 public:
  virtual ~ICompositionEngine() = default;

  virtual Result<GainMap> CompensateExposure(const GlobalSolution&,
                                             std::span<const FrameRef>) = 0;

  // Disagreement between a cell's own candidates localises movers directly (docs/04 §4.5): the
  // burst we kept for selection is also the ghost detector.
  virtual Result<GhostReport> DetectGhosts(const GlobalSolution&,
                                           std::span<const Candidate> allCandidates) = 0;

  virtual Result<SeamMap> FindSeams(const GlobalSolution&, const GainMap&, const GhostReport&,
                                    const BuildSpec&) = 0;

  // Tiled so that a retake re-blends a handful of tiles rather than the whole sphere.
  virtual Result<FrameRef> BlendTile(const GlobalSolution&, const GainMap&, const SeamMap&,
                                     const BuildSpec&, int32_t tileX, int32_t tileY) = 0;

  virtual Result<FrameRef> RenderPreview(const GlobalSolution&, const GainMap&,
                                         int32_t maxWidth) = 0;
};

}  // namespace sphanorama
