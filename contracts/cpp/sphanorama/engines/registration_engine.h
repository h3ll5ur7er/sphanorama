#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V7 — how frames are aligned.
class IRegistrationEngine {
 public:
  virtual ~IRegistrationEngine() = default;

  // Features found in one frame. **The two frames in the answer belong to the caller**, who must
  // `Forget` each — `FeatureSet` says what they hold and ADR 0051 says why they are frames. A
  // `count` of zero means nothing was allocated and there is nothing to forget.
  //
  // The frame handed in is left alone, except that reading it pins it, and pinning faults a spilled
  // frame back into the heap. An implementation may refuse: `Unsupported` for a format with no luma
  // plane to read, `InvalidArgument` for geometry the store is not holding, `NotFound` for a handle
  // naming no frame, and `FrameStoreExhausted` when there is no room for the answer.
  virtual Result<FeatureSet> ExtractFeatures(const FrameRef&) = 0;

  // The sensor pose enters here as a prior that seeds and bounds the search — never as truth.
  virtual Result<PairwiseResult> EstimatePairwise(const FeatureSet& a, const FeatureSet& b,
                                                  const Quat& prior) = 0;

  virtual Result<GlobalSolution> Refine(std::span<const PairwiseResult>,
                                        std::span<const PoseSample> priors,
                                        const Intrinsics& initial) = 0;
};

}  // namespace sphanorama
