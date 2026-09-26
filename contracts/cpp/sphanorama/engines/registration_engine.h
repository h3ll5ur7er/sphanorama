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
  // the same reason and puts it to a different use — where its fit of the focal length starts, and
  // the lens its answer is expressed under when there is no fit (ADR 0066) — which is why that one
  // is named `initial` and this one is not.
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

  // One consistent set of absolute rotations from the pairwise ones and the sensor priors.
  //
  // The pairs are relative and independently estimated; the priors are absolute and out by degrees.
  // Where the accepted pairs join the frames, the pairs decide how they sit relative to each other
  // and the priors decide which way they face, which is the one thing the pairs cannot say. The
  // priors also pull on that shape, each on its own frame, at a weight far below any pair's — so they
  // barely correct it: an accepted pair ten degrees wrong on an open chain is drawn back by a few
  // thousandths of a degree at a hundred inliers and a few hundredths at fifteen, and the error
  // figures read clean over it, since nothing else the pairs say contradicts it. Only another pair
  // does, such as a ring's closing one. Where the pairs do not join the frames, nothing but the
  // priors relates one piece to the other, and the seam between them is placed to the priors' degrees
  // — `GlobalSolution::pieces` says how many pieces there were. That is why a ring's closing pair is
  // worth having: a chain throws it away, and it is the measurement of how far the chain drifted.
  //
  // **Each prior names its frame**, and the priors are the frame set: a pair naming a frame with no
  // prior is refused rather than guessed at. Only accepted pairs are used — `accepted` is exactly
  // the question "should a global solve use this edge" (ADR 0056) — so what an unaccepted pair
  // carries, its rotation and its counts, can neither move the answer nor refuse it. The frames it
  // names are checked all the same: a stranger is a caller and a capture disagreeing about which
  // frames exist. A prior counts only if its `confidence` is above zero, since zero is a direction
  // relative to wherever the sensor started (ADR 0041), and averaged in with anchored priors would
  // turn the whole answer toward that accident. Zero is the only way to say there is no prior.
  //
  // **The focal length is fitted where a loop of pairs can see it**, and nothing else of the lens
  // is (ADR 0066). A pair cannot see it — rotated under a focal length a few percent out, its
  // pixels fit as well as under the right one — but a loop can: the pairs agree around it only under
  // the true focal length. So `Refine` searches the scale of `fx` and `fy` together for the one under
  // which the pairs, each refitted from its `inlierMatches`, leave the solve least, and returns
  // `initial` so scaled in `GlobalSolution::intrinsics` with `lensFitted` set. Only frames the solve
  // places take part, and every scale is scored on the same matches — those with a direction at the
  // shortest focal length searched, and where one loses its direction at a scale the search tries,
  // which tangential distortion allows, there is no fit. Only the scales it tries: one that loses it
  // in a window between two of them goes unseen, and the fit is taken on matches that all have a
  // direction under the lens it returns. Where the accepted pairs among placed frames close no loop,
  // where one of them keeps fewer than three such matches, where any trial could not be scored on
  // them, where the cost does not rise on both sides of its least, where the lens gives the frame's
  // corner no direction, or where the least is not precise — how far the pairs' noise could move it
  // and how far a lens misread by a thousandth of the focal length at the frame's corner does,
  // within two tenths of a percent together — or where that is not less than the
  // `focalUncertainty` of the lens handed in, `initial` comes back as given, every field of it, and
  // `lensFitted` is false. A lens read from a reported field of view is a guess, and any precise fit
  // replaces it; a lens a device kept from earlier captures can be surer than a weak loop's fit, and
  // is not replaced by one (ADR 0067). A fitted lens carries its own figure in `focalUncertainty`, and
  // wherever the least is precise, taken or not, `GlobalSolution::focalScale` says where it lies,
  // which is what a kept lens is amended with. Loops too weak to see the focal length
  // past the pairs' noise or past that misreading are refused this way, however many of them there
  // are and however far their least lies from the lens handed in. Where the lens is fitted the
  // rotations are the refitted pairs' solve, not the one their own `relativeRotation`s give; where
  // it is not, they are that one, read as measured under `initial` — so pairs estimated under
  // another lens give rotations that lens's error, whether or not a precise least was found (ADR
  // 0067).
  //
  // Refusals: `InvalidArgument` for no priors, an invalid or repeated frame among them, a prior
  // whose confidence is outside [0, 1] or whose orientation is not a rotation while its confidence
  // claims one, a lens `IsUsableLens` would not accept or whose `focalUncertainty` is not a figure
  // at all — NaN, or below zero — a pair naming a frame with no prior or the
  // same frame twice, or an accepted pair whose rotation is not one — including one never written,
  // which `PairwiseResult` defaults so as to be refused — or whose counts no engine fills in: no
  // inliers, or fewer correspondences than inliers. `FailedPrecondition` when every prior's
  // confidence is zero, because then nothing says which way the reconstruction faces.
  // An accepted pair whose `inlierMatches` are not empty and are not as many as its `inliers`, or
  // are not all finite pixels, is `InvalidArgument` too.
  // `Unsupported` from `NullRegistrationEngine`, which has no pairs to solve with. `Internal` for a
  // refusal from the solver these checks did not anticipate, which nothing short of 2^31 priors or
  // accepted pairs reaches today, and for anything the implementation's own machinery throws — the
  // OpenCV-backed one converts OpenCV's exceptions and the standard library's, allocation failure
  // among them, as its other methods do. A malformed prior or pair is `InvalidArgument` whatever the other priors say. A
  // frame with no prior is not a refusal: it is placed through its pairs, or named in
  // `droppedFrames`.
  virtual Result<GlobalSolution> Refine(std::span<const PairwiseResult> pairs,
                                        std::span<const FramePrior> priors,
                                        const Intrinsics& initial) = 0;
};

}  // namespace sphanorama
