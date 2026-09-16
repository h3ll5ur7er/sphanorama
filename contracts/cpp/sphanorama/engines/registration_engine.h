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
  // **The set it answers with carries this implementation's `extractor` stamp**, which is what makes
  // `EstimatePairwise`'s refusal of a foreign set satisfiable: a caller may pair only sets that came
  // from the same engine. Promised here rather than left to the field's own comment, because an
  // implementation could otherwise honour the letter of both methods, return the default identity
  // from this one, and refuse every pair built from its own sets.
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
  // compute placement or pixel residency arrives as an argument. `Refine` takes an `Intrinsics` for
  // the same reason and puts it to a different use — it is where a solve starts and a refined lens
  // comes back in the result, which is why that one is named `initial` and this one is not. Both
  // are `const`: an earlier version of this sentence contrasted them on constness, which was never
  // the difference.
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
  // - `InvalidArgument` — the inputs could not be read as a pair: an empty feature set, a set this
  //   engine's extractor did not produce (`FeatureSet::extractor`, ADR 0058), a prior that is not a
  //   usable rotation, a lens that cannot project, a frame whose declared rows do not fit the bytes
  //   it holds, or two sets whose descriptor rows are different widths.
  // - **`Pin`'s own status, whole** — `code`, `detail` and `component` — when a frame could not be
  //   pinned. So a caller branching on the code sees what the store said, and a human reading the
  //   component sees the store that said it rather than the engine that was asking.
  //
  //   This line has now been wrong in both directions, which is worth leaving on the record because
  //   the second was subtler than the first. It first claimed the status came back "unchanged"; a
  //   reviewer disproved that by probe (`component = "FeatureRegistrationEngine"` over the store's
  //   `code = 2`), so it was rewritten to say the component is replaced. Then the engine was changed
  //   to return the store's status whole — and this line was not, so it went on describing the
  //   behaviour it had just been corrected *to* describe, one commit after that stopped being true.
  //   `core/test/engines/registration_engine_test.cpp` asserts the current behaviour directly
  //   (`EXPECT_NE(pair.status.component, "FeatureRegistrationEngine")`), which is the only reason
  //   the contract and the code could drift apart without a test noticing: the test was checking the
  //   code, and nothing checks this sentence.
  // - `Internal` when the compute library throws, which is how it reports what it does not model.
  // - `Unsupported` — from `NullRegistrationEngine`, which is what a build without OpenCV gets
  //   (ADR 0052). It was missing from this list until a reviewer read the list against the
  //   composition roots rather than against the implementation in front of it, and `ExtractFeatures`
  //   documents it at the top of this file.
  //
  //   **What that means today needs stating carefully, because the first version of this bullet got
  //   it backwards.** It said `Unsupported` is "the only status this method returns in any shipping
  //   browser build", which reads as a fact about what browsers see. No manager takes an
  //   `IRegistrationEngine` at all, so nothing outside tests calls this method in any build. The
  //   accurate statement is conditional: the first composition root that wires this up will get the
  //   null engine everywhere OpenCV is absent, which today is every browser build, and `Unsupported`
  //   is what it will see. A caller written against the codes above and not this one is writing for
  //   the build that does not exist yet.
  //
  // An `Ok` result is not the same as an accepted one: see `PairwiseResult::accepted`, which is
  // false when a rotation was found and a minority of the correspondences agree with it (ADR 0056).
  virtual Result<PairwiseResult> EstimatePairwise(const FeatureSet& a, const FeatureSet& b,
                                                  const Quat& prior, const Intrinsics& lens) = 0;

  // **Nothing implements this yet, and the contract has to say so.** Both engines refuse with
  // `Unsupported` — `FeatureRegistrationEngine` with "the global refinement is a later increment",
  // `NullRegistrationEngine` with "bundle adjustment is Phase 2" — so a caller branching on
  // `StatusCode` gets exactly one code from this method in every build that exists today.
  //
  // Stated here because the `Unsupported` bullet above is scoped to `EstimatePairwise` *and*
  // attributed to "`NullRegistrationEngine`, which is what a build without OpenCV gets". A reader
  // generalising it across the interface concludes that the OpenCV engine implements this one. It
  // does not, and the header was silent on the only method where that inference is wrong: every
  // other method on this interface has a paragraph, and this had a bare declaration.
  //
  // **What it will do when it exists.** Take the pairwise rotations, which are relative and
  // independently estimated, and the sensor priors, which are absolute and drift; solve for one
  // consistent set of absolute rotations plus a refined lens. `initial` is named for its role in
  // that: it is where the solve *starts*, and a `GlobalSolution` carries the lens it *ends* with.
  // The parameter is `const` because this engine is stateless per session and improves a value by
  // returning a new one, not by writing through its argument — which is the distinction the
  // paragraph on `EstimatePairwise`'s lens draws, and draws badly by contrasting `const` against
  // `initial` as though those were alternatives. Both parameters are `const Intrinsics&`. The
  // difference is what the caller does with the answer.
  //
  // The first `PairwiseResult` span is unnamed because a refusal reads none of it. Name it when
  // something reads it (ADR 0054: a signature that cannot honour its own return type is a gap a
  // stub hides, and this one is declared honestly and refuses honestly until it can).
  virtual Result<GlobalSolution> Refine(std::span<const PairwiseResult>,
                                        std::span<const PoseSample> priors,
                                        const Intrinsics& initial) = 0;
};

}  // namespace sphanorama
