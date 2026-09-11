#pragma once

#include "sphanorama/resource_access/frame_store_access.h"
#include "sphanorama/engines/registration_engine.h"

namespace sphanorama {

// Which detector finds the features. **A parameter, not a preference** — V7 names ORB, AKAZE and
// SIFT together because speed against repeatability on *this* content, at *this* frame size, is a
// measurement rather than an opinion. SIFT's patent expired in 2020 and it has been in `features2d`
// since OpenCV 4.4, so it costs no new dependency and the reason it was once excluded is gone.
//
// It is a construction-time choice rather than a contract type, so whoever composes this engine
// picks one and no caller above the engine layer learns that detectors exist. Today that is the
// tests: the only composition root in the repository is the WASM runtime, which has no OpenCV and
// holds the null engine unconditionally (ADR 0052). The native client that will choose a detector
// in earnest is the one that runs the accuracy harness, and it does not exist yet.
enum class FeatureDetector { Orb, Akaze, Sift };

// The most features any detector may return for one frame.
//
// Named here rather than kept private because a test has to build the *same* detector to check this
// engine against, and a cap is part of what "the same detector" means: a detector asked for 50
// features does not return the first 50 an uncapped one would, since `retainBest` selects by
// response across the whole set. A test that restated the number instead would pass today and fail
// the day somebody tuned it, blaming the engine.
//
// Without it two of the three are unbounded: `cv::ORB::create()` caps itself at 500, while
// `cv::SIFT::create()` and `cv::AKAZE::create()` retain everything — 1,328 and 2,547 on this
// repository's test texture at 768 square. It bounds the allocation, and it is also what makes
// comparing the three a measurement rather than a comparison between one asked for 500 features and
// another asked for all of them.
inline constexpr int kMaxFeaturesPerFrame = 500;

// V7 — feature extraction, matching and global refinement over OpenCV.
//
// Compiled only when `SPHANORAMA_WITH_OPENCV` is on; `NullRegistrationEngine` is what a WASM build
// gets instead (ADR 0052). It reads pixels and allocates frames, so it holds `IFrameStoreAccess` —
// one of the two resource accesses an engine may touch.
//
// This increment implements `ExtractFeatures` only. `EstimatePairwise` and `Refine` refuse, for the
// reason the null engine gives: there is no honest minimal version of matching, and a stub
// returning identity rotations would produce a panorama that looks stitched and is not.
class FeatureRegistrationEngine final : public IRegistrationEngine {
 public:
  FeatureRegistrationEngine(IFrameStoreAccess& frames, FeatureDetector detector)
      : frames_(frames), detector_(detector) {}

  Result<FeatureSet> ExtractFeatures(const FrameRef& frame) override;
  Result<PairwiseResult> EstimatePairwise(const FeatureSet& a, const FeatureSet& b,
                                          const Quat& prior) override;
  Result<GlobalSolution> Refine(std::span<const PairwiseResult> pairs,
                                std::span<const PoseSample> priors,
                                const Intrinsics& initial) override;

 private:
  // The whole of extraction, written as if exceptions did not exist — every failure it knows about
  // is a `Result`. `ExtractFeatures` wraps it in the one `try` this component has, because OpenCV
  // reports failures we do not know about by throwing (ADR 0047, ADR 0052).
  Result<FeatureSet> Extract(const FrameRef& frame);

  IFrameStoreAccess& frames_;
  FeatureDetector detector_;
};

}  // namespace sphanorama
