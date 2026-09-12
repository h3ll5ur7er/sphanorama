#pragma once
#include <array>

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
// `Count` is not a detector. It is here so the list below can be *checked* rather than remembered.
//
// The first version of this pair claimed that deriving the test parameters from one list meant "a
// fourth detector reaches every test without anyone remembering to widen a `Values(...)` nothing
// checks". A reviewer showed that was still false: they added a fourth enumerator, satisfied the
// three `default`-less switches the `-Werror` build demanded, and the suite then reported 713
// passing tests while running `EveryDetector/Extraction` against three detectors and executing the
// new one nowhere. An array with its size written into its type does not grow when an enum does.
//
// The cost is a value that is not a detector, which is the sentinel shape this codebase is
// otherwise strict about. It is bounded deliberately: `Make()` answers null for it and
// `ExtractFeatures` turns that into `Unsupported` — a path that already existed for a detector it
// could not build, and that a test now reaches.
enum class FeatureDetector { Orb, Akaze, Sift, Count };

// Every detector, once, beside the enum. The `-Werror` switches send you to this header when an
// enumerator appears; the assertion below is what stops you leaving again without extending this.
inline constexpr std::array<FeatureDetector, 3> kAllFeatureDetectors{
    FeatureDetector::Orb, FeatureDetector::Akaze, FeatureDetector::Sift};
static_assert(kAllFeatureDetectors.size() == static_cast<size_t>(FeatureDetector::Count),
              "a detector was added to FeatureDetector and not to kAllFeatureDetectors, so the "
              "parameterised tests would silently go on covering the old ones");

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
// `ExtractFeatures` and `EstimatePairwise` are implemented; `Refine` still refuses, for the reason
// the null engine gives: there is no honest minimal version of a global solve, and a stub returning
// an empty solution would produce a panorama that looks stitched and is not.
//
// `EstimatePairwise` matches by Lowe's ratio test, lifts both keypoint sets to bearings through
// `camera_model` — which is why it needs the lens, ADR 0054 — and fits the rotation most of the
// correspondences agree on by RANSAC over minimal samples, refitting on the inliers by Kabsch. The
// sensor prior is scored as one hypothesis among the sampled ones rather than descended from, so a
// right sensor wins immediately and a wrong one loses to the pixels.
class FeatureRegistrationEngine final : public IRegistrationEngine {
 public:
  FeatureRegistrationEngine(IFrameStoreAccess& frames, FeatureDetector detector)
      : frames_(frames), detector_(detector) {}

  Result<FeatureSet> ExtractFeatures(const FrameRef& frame) override;
  Result<PairwiseResult> EstimatePairwise(const FeatureSet& a, const FeatureSet& b,
                                          const Quat& prior, const Intrinsics& lens) override;
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
