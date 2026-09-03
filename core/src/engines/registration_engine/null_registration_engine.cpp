#include "engines/registration_engine/null_registration_engine.h"

namespace sphanorama {
namespace {
constexpr const char* kComponent = "NullRegistrationEngine";
}

Result<FeatureSet> NullRegistrationEngine::ExtractFeatures(const FrameRef&) {
  return Err<FeatureSet>(StatusCode::Unsupported, kComponent, "feature extraction is Phase 2");
}

Result<PairwiseResult> NullRegistrationEngine::EstimatePairwise(const FeatureSet&,
                                                                const FeatureSet&, const Quat&) {
  return Err<PairwiseResult>(StatusCode::Unsupported, kComponent, "matching is Phase 2");
}

Result<GlobalSolution> NullRegistrationEngine::Refine(std::span<const PairwiseResult>,
                                                      std::span<const PoseSample>,
                                                      const Intrinsics&) {
  return Err<GlobalSolution>(StatusCode::Unsupported, kComponent,
                             "bundle adjustment is Phase 2");
}

}  // namespace sphanorama
