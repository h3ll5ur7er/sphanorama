#pragma once

#include "sphanorama/engines/composition_engine.h"
#include "sphanorama/resource_access/frame_store_access.h"

namespace sphanorama {

// V8's first real answer: the preview, with each direction of the sphere coloured by the frame
// whose optical axis is nearest it among the frames that see it (ADR 0068).
//
// Nearest centre rather than a blend, because a preview's job is to show the registration, and a
// blend hides it: a rotation a degree out shows here as a step at the boundary between two frames,
// where a feather would draw it as a soft double image that reads as blur. And because where the
// registration is right there is nothing to hide — two frames of one scene agree, so the one chosen
// is the same colour either way. Exposure, seams and multi-band blending are `BlendTile`'s, and the
// methods that would make them answer `Unsupported` until they are built.
//
// It reads pixels and allocates its answer, so it holds `IFrameStoreAccess`, one of the two
// resource accesses an engine may touch (docs/03 §3.3 rule 5).
class NearestCentreCompositionEngine final : public ICompositionEngine {
 public:
  explicit NearestCentreCompositionEngine(IFrameStoreAccess& frames) : frames_(frames) {}

  Result<GainMap> CompensateExposure(const GlobalSolution& solution,
                                     std::span<const FrameRef> frames) override;
  Result<GhostReport> DetectGhosts(const GlobalSolution& solution,
                                   std::span<const Candidate> allCandidates) override;
  Result<SeamMap> FindSeams(const GlobalSolution& solution, std::span<const FrameRef> frames,
                            const GainMap& gains, const GhostReport& ghosts,
                            const BuildSpec& spec) override;
  Result<FrameRef> BlendTile(const GlobalSolution& solution, std::span<const FrameRef> frames,
                             const GainMap& gains, const SeamMap& seams, const BuildSpec& spec,
                             int32_t tileX, int32_t tileY) override;
  Result<FrameRef> RenderPreview(const GlobalSolution& solution, std::span<const FrameRef> frames,
                                 const GainMap& gains, int32_t maxWidth) override;

 private:
  IFrameStoreAccess& frames_;
};

}  // namespace sphanorama
