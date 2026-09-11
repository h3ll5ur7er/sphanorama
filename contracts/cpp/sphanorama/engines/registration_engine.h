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
  // The frame handed in is left alone, except that reading it pins it — and pinning faults a
  // spilled frame back into the heap and leaves it resident, which is a cost a caller working
  // through cold frames should expect.
  //
  // An implementation may refuse with `Unsupported` (a format with no luma plane to read, or an
  // implementation that does no registration at all — the null one refuses everything this way),
  // `InvalidArgument` (geometry the store is not holding), `NotFound` (a handle naming no frame),
  // `FrameStoreExhausted` (no room for the answer), or `Internal` (the implementation's own
  // machinery refused; the OpenCV-backed one reports OpenCV's assertions this way).
  virtual Result<FeatureSet> ExtractFeatures(const FrameRef&) = 0;

  // The sensor pose enters here as a prior that seeds and bounds the search — never as truth.
  //
  // **The two `FeatureSet`s belong to the caller, and this call does not consume them.** It may pin
  // and release their frames; it must not `Forget` any of the four, and it must not `Forget` them on
  // a refusal either. The caller may estimate the same pair twice, or one set against several
  // others, so an implementation that tidied up after itself would destroy the second call's input.
  //
  // Worth saying explicitly because the shape invites the opposite. A reviewer noted that the
  // ownership rule above was written for the two frames coming *out* of `ExtractFeatures` while
  // nothing was said about the four going *in* here — and this is the method with the obvious-
  // looking reason to release what it was handed. Until ADR 0051 these were `BufferId`s naming
  // nothing, so the question could not be asked; they are four live store allocations per call now.
  virtual Result<PairwiseResult> EstimatePairwise(const FeatureSet& a, const FeatureSet& b,
                                                  const Quat& prior) = 0;

  virtual Result<GlobalSolution> Refine(std::span<const PairwiseResult>,
                                        std::span<const PoseSample> priors,
                                        const Intrinsics& initial) = 0;
};

}  // namespace sphanorama
