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
  //
  // **The lens is a parameter because the answer cannot be computed without it.** Under pure
  // rotation the two views are related by a homography `H = K R inverse(K)`, so matched pixels
  // determine `H` and nothing else; recovering the `R` this method is declared to return needs `K`.
  // The first attempt at implementing this discovered that the signature could not honour its own
  // return type, which is the kind of gap a stub hides (ADR 0054).
  //
  // Passed rather than held, because engines are stateless per session and everything that is not
  // compute placement or pixel residency arrives as an argument. `Refine` already takes an
  // `Intrinsics` for the same reason, and takes it as the value it goes on to *improve*; here it is
  // read and not improved, which is why this one is `const` and that one is named `initial`.
  // **How it refuses**, which belongs here rather than in an implementation: a caller branching on
  // `StatusCode` can only do so against what the contract promises, and this method is the first in
  // the repository to return `RegistrationFailed` at all.
  //
  // - `RegistrationFailed` — the two frames did not register. Either too few correspondences
  //   survived matching to fit a rotation, or no rotation was agreed on by enough of them. Both are
  //   the same fact to a caller (these frames do not go together) and the detail says which, for a
  //   human. It is deliberately **not** `NotFound`, which `IFrameStoreAccess` uses for a handle
  //   naming no frame: one code meaning "the pixels disagree" and "that frame does not exist" is a
  //   code nobody can branch on.
  // - `InvalidArgument` — the inputs could not be read as a pair: an empty feature set, a prior that
  //   is not a usable rotation, a lens that cannot project, a frame whose declared rows do not fit
  //   the bytes it holds, or two sets made by different detectors.
  // - `Pin`'s own `code` and `detail` when a frame could not be pinned — but **not** its
  //   `component`, which is replaced by this engine's. So a caller branching on the code sees what
  //   the store said and a human reading the component sees who was holding it at the time. An
  //   earlier version of this line claimed the status came back "unchanged", which a reviewer
  //   disproved by probe: `component = "FeatureRegistrationEngine"` over the store's `code = 2`
  //   and `detail = "no such frame"`.
  // - `Internal` when the compute library throws, which is how it reports what it does not model.
  //
  // An `Ok` result is not the same as an accepted one: see `PairwiseResult::accepted`, which is
  // false when a rotation was found and a minority of the correspondences agree with it (ADR 0056).
  virtual Result<PairwiseResult> EstimatePairwise(const FeatureSet& a, const FeatureSet& b,
                                                  const Quat& prior, const Intrinsics& lens) = 0;

  virtual Result<GlobalSolution> Refine(std::span<const PairwiseResult>,
                                        std::span<const PoseSample> priors,
                                        const Intrinsics& initial) = 0;
};

}  // namespace sphanorama
