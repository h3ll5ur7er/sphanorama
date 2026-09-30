#include "engines/registration_engine/null_registration_engine.h"

namespace sphanorama {
namespace {
constexpr const char* kComponent = "NullRegistrationEngine";
}

Result<FeatureSet> NullRegistrationEngine::ExtractFeatures(const FrameRef&) {
  // Not "needs OpenCV": since ADR 0069 the browser's build has it and its runtime still holds this
  // engine, because nothing composes the real one yet. What is missing is the engine, either way.
  return Err<FeatureSet>(StatusCode::Unsupported, kComponent,
                         "feature extraction is unavailable: this build has no registration "
                         "engine");
}

Result<PairwiseResult> NullRegistrationEngine::EstimatePairwise(const FeatureSet&,
                                                                const FeatureSet&, const Quat&,
                                                                const Intrinsics&) {
  // "not built here" rather than "not written": `FeatureRegistrationEngine` matches and registers a
  // pair, and this engine is what a build gets when nothing composes that one — without OpenCV
  // (ADR 0052), or with it and no selection yet (ADR 0069). Saying it is Phase 2 work stopped being
  // true when that landed, and a reader would go looking for an unwritten feature.
  return Err<PairwiseResult>(StatusCode::Unsupported, kComponent,
                             "pairwise registration is unavailable: this build has no registration "
                             "engine");
}

Result<GlobalSolution> NullRegistrationEngine::Refine(std::span<const PairwiseResult>,
                                                      std::span<const FramePrior>,
                                                      const Intrinsics&) {
  return Err<GlobalSolution>(StatusCode::Unsupported, kComponent,
                             "a global solve needs registered pairs, and this build has no "
                             "registration engine to register them");
}

}  // namespace sphanorama
