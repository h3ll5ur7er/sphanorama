#include "engines/composition_engine/null_composition_engine.h"

namespace sphanorama {
namespace {
constexpr const char* kComponent = "NullCompositionEngine";
}

Result<GainMap> NullCompositionEngine::CompensateExposure(const GlobalSolution&,
                                                          std::span<const FrameRef>) {
  return Err<GainMap>(StatusCode::Unsupported, kComponent, "exposure compensation is Phase 2");
}

Result<GhostReport> NullCompositionEngine::DetectGhosts(const GlobalSolution&,
                                                        std::span<const Candidate>) {
  return Err<GhostReport>(StatusCode::Unsupported, kComponent, "ghost detection is Phase 3");
}

Result<SeamMap> NullCompositionEngine::FindSeams(const GlobalSolution&, const GainMap&,
                                                 const GhostReport&, const BuildSpec&) {
  return Err<SeamMap>(StatusCode::Unsupported, kComponent, "seam finding is Phase 2");
}

Result<FrameRef> NullCompositionEngine::BlendTile(const GlobalSolution&, const GainMap&,
                                                  const SeamMap&, const BuildSpec&, int32_t,
                                                  int32_t) {
  return Err<FrameRef>(StatusCode::Unsupported, kComponent, "blending is Phase 2");
}

Result<FrameRef> NullCompositionEngine::RenderPreview(const GlobalSolution&, const GainMap&,
                                                      int32_t) {
  return Err<FrameRef>(StatusCode::Unsupported, kComponent, "preview rendering is Phase 2");
}

}  // namespace sphanorama
