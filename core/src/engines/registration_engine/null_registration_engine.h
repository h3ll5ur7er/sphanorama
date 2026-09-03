#pragma once

#include "sphanorama/engines/registration_engine.h"

namespace sphanorama {

// Null object for V7: every call returns Unsupported.
//
// There is no honest minimal version of feature matching. A stub returning identity rotations
// would produce a panorama that looks stitched and is not, which is worse than one that refuses.
class NullRegistrationEngine final : public IRegistrationEngine {
 public:
  Result<FeatureSet> ExtractFeatures(const FrameRef& frame) override;
  Result<PairwiseResult> EstimatePairwise(const FeatureSet& a, const FeatureSet& b,
                                          const Quat& prior) override;
  Result<GlobalSolution> Refine(std::span<const PairwiseResult> pairs,
                                std::span<const PoseSample> priors,
                                const Intrinsics& initial) override;
};

}  // namespace sphanorama
