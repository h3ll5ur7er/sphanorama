#include "engines/registration_engine/null_registration_engine.h"

namespace sphanorama {
namespace {
constexpr const char* kComponent = "NullRegistrationEngine";
}

Result<FeatureSet> NullRegistrationEngine::ExtractFeatures(const FrameRef&) {
  // Stale since #68 in the same way `EstimatePairwise`'s message was: extraction is written, and
  // what this build lacks is the library it needs.
  return Err<FeatureSet>(StatusCode::Unsupported, kComponent,
                         "feature extraction needs OpenCV, which this build does not have");
}

Result<PairwiseResult> NullRegistrationEngine::EstimatePairwise(const FeatureSet&,
                                                                const FeatureSet&, const Quat&,
                                                                const Intrinsics&) {
  // "not built here" rather than "not written": `FeatureRegistrationEngine` matches and registers a
  // pair, and this engine is what a build without OpenCV gets instead (ADR 0052). Saying it is Phase
  // 2 work stopped being true when that landed, and a browser user reading the message would go
  // looking for an unwritten feature rather than a missing dependency.
  return Err<PairwiseResult>(StatusCode::Unsupported, kComponent,
                             "pairwise registration needs OpenCV, which this build does not have");
}

Result<GlobalSolution> NullRegistrationEngine::Refine(std::span<const PairwiseResult>,
                                                      std::span<const PoseSample>,
                                                      const Intrinsics&) {
  return Err<GlobalSolution>(StatusCode::Unsupported, kComponent,
                             "bundle adjustment is Phase 2");
}

}  // namespace sphanorama
