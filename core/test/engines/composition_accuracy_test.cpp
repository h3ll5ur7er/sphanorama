/**
 * **Does the preview draw the world the frames were taken of?** Frames rendered from a photograph,
 * composed with the rotations they were rendered at, compared with the photograph sampled where
 * each of the preview's pixels looks (ADR 0068).
 *
 * The reference comes from the generator, which samples the photograph directly, not from anything
 * in C++: a reference drawn through the code under test would agree with it about everything the
 * two got wrong together (ADR 0050). Rendered here rather than committed and skipped without `uv`,
 * as the registration measurement is (ADR 0055).
 *
 * With the true rotations this measures the compositor alone. It is not gauge-free — a common
 * rotation moves the whole preview against the reference — so a registered solution has its gauge
 * taken off before it is composed, with the alignment the rotation scorer computes.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <vector>

#include "engines/composition_engine/nearest_centre_composition_engine.h"
#include "engines/registration_engine/feature_registration_engine.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "support/rendered_dataset.h"
#include "support/rotation_scoring.h"
#include "support/synthetic_dataset.h"
#include "support/wrong_priors.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

using test::Rendered;
using test::World;

constexpr int kFrames = 12;
// The photograph's own width, so the preview's pixel centres are the photograph's and the reference
// is its pixels, unresampled.
constexpr int32_t kPreviewWidth = 1024;

// Mean absolute error over every covered colour component, in bytes of the signed encoding.
// Measured at 1.193 with the true rotations: the floor is two bilinear samples, photograph to frame
// and frame to preview, against none. Measured at 2.84 with alternate frames turned a tenth of a
// degree either way, which is under a third of a preview pixel and a fifth of the 0.5-degree
// threshold registration exits on — so the bound sits between the two, and a registration error
// the exit criterion allows is one this can see.
constexpr double kMeanErrorBound = 2.0;
// The tail, where the photograph's edges are: measured at 11.
constexpr int kP99ErrorBound = 16;

struct Comparison {
  double covered = 0.0;
  double equatorCovered = 0.0;
  bool poleCovered = false;
  double meanError = 0.0;
  int p99Error = 0;
};

class CompositionAccuracy : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    rendered_ = std::make_unique<Rendered>(kFrames, 640, 480, World::Photograph, kPreviewWidth);
  }
  static void TearDownTestSuite() { rendered_.reset(); }

  void SetUp() override {
    ASSERT_FALSE(rendered_->inputMissing()) << rendered_->why();
    if (!rendered_->ok()) GTEST_SKIP() << rendered_->why();
    Result<SyntheticDataset> dataset = LoadSyntheticDataset(store_, rendered_->path());
    ASSERT_TRUE(dataset.ok()) << dataset.status.detail;
    for (const SyntheticFrame& frame : dataset.value.frames) owned_.push_back(frame.frame);
    ASSERT_EQ(dataset.value.frames.size(), static_cast<size_t>(kFrames));
    Result<FrameRef> reference =
        LoadSyntheticReference(store_, rendered_->path(), kPreviewWidth, kPreviewWidth / 2);
    ASSERT_TRUE(reference.ok()) << reference.status.detail;
    owned_.push_back(reference.value);
    reference_ = reference.value;
    truth_.intrinsics = dataset.value.lens;
    for (const SyntheticFrame& frame : dataset.value.frames) {
      frames_.push_back(frame.frame);
      truth_.frames.push_back(frame.frame.id);
      truth_.rotations.push_back(frame.trueRotation);
    }
  }

  // Every frame goes back on every path out, a failed assertion included, and the store is then
  // empty: a preview left behind is a leak nothing else would report.
  void TearDown() override {
    for (const FrameRef& frame : owned_) {
      // The store's own sentence: a pin left behind and a handle it never allocated both refuse,
      // and only the detail tells them apart.
      const Status forgotten = store_.Forget(frame);
      EXPECT_TRUE(forgotten.ok()) << "frame " << frame.id.value << ": " << forgotten.detail;
    }
    Result<FrameStoreBudget> budget = store_.Budget();
    ASSERT_TRUE(budget.ok());
    EXPECT_EQ(budget.value.heapUsedBytes, 0);
  }

  Comparison Compose(const GlobalSolution& solution) {
    Comparison result;
    Result<FrameRef> preview = engine_.RenderPreview(solution, frames_, {}, kPreviewWidth);
    EXPECT_TRUE(preview.ok()) << preview.status.detail;
    if (!preview.ok()) return result;
    owned_.push_back(preview.value);
    const FrameRef& drawn = preview.value;
    // Read through the reference's rows too, so a preview of another shape stops here rather than
    // reading past them.
    EXPECT_EQ(drawn.width, reference_.width);
    EXPECT_EQ(drawn.height, reference_.height);
    if (drawn.width != reference_.width || drawn.height != reference_.height) return result;
    Result<std::span<uint8_t>> a = store_.Pin(drawn);
    Result<std::span<uint8_t>> b = store_.Pin(reference_);
    EXPECT_TRUE(a.ok() && b.ok());
    if (a.ok() && b.ok()) {
      std::vector<int> errors;
      int64_t covered = 0;
      int64_t equator = 0;
      for (int32_t y = 0; y < drawn.height; ++y) {
        for (int32_t x = 0; x < drawn.width; ++x) {
          const uint8_t* p = a.value.data() + static_cast<size_t>(y) * drawn.stride + x * 4;
          const uint8_t* r = b.value.data() + static_cast<size_t>(y) * reference_.stride + x * 4;
          if (p[3] != 255) continue;
          ++covered;
          if (y == drawn.height / 2) ++equator;
          if (y == 0 || y == drawn.height - 1) result.poleCovered = true;
          for (int c = 0; c < 3; ++c) errors.push_back(std::abs(p[c] - r[c]));
        }
      }
      result.covered = static_cast<double>(covered) / (drawn.width * drawn.height);
      result.equatorCovered = static_cast<double>(equator) / drawn.width;
      if (!errors.empty()) {
        double sum = 0.0;
        for (const int error : errors) sum += error;
        result.meanError = sum / static_cast<double>(errors.size());
        std::sort(errors.begin(), errors.end());
        result.p99Error = errors[errors.size() * 99 / 100];
      }
    }
    if (a.ok()) {
      EXPECT_TRUE(store_.Release(drawn).ok());
    }
    if (b.ok()) {
      EXPECT_TRUE(store_.Release(reference_).ok());
    }
    return result;
  }

  static std::unique_ptr<Rendered> rendered_;
  MemoryFrameStoreAccess store_{1 << 28};
  NearestCentreCompositionEngine engine_{store_};
  std::vector<FrameRef> owned_;
  std::vector<FrameRef> frames_;
  FrameRef reference_;
  GlobalSolution truth_;
};

std::unique_ptr<Rendered> CompositionAccuracy::rendered_;

TEST_F(CompositionAccuracy, TheTruthComposesToThePhotograph) {
  const Comparison drawn = Compose(truth_);
  std::printf("[composition] frames=%d width=%d covered=%.4f mean=%.3f p99=%d\n", kFrames,
              kPreviewWidth, drawn.covered, drawn.meanError, drawn.p99Error);

  // A ring at the horizon sees all of it and neither pole: 50 degrees of the lens's 180 of
  // latitude, less where a frame's corners fall short of its centre column's reach.
  EXPECT_EQ(drawn.equatorCovered, 1.0);
  EXPECT_FALSE(drawn.poleCovered);
  EXPECT_NEAR(drawn.covered, 50.0 / 180.0, 0.01);

  EXPECT_LE(drawn.meanError, kMeanErrorBound);
  EXPECT_LE(drawn.p99Error, kP99ErrorBound);
}

// The bound is only worth something if a registration error the exit criterion allows fails it:
// alternate frames a tenth of a degree either side of the truth, seams everywhere and nothing moved
// far.
TEST_F(CompositionAccuracy, ATenthOfADegreeIsVisible) {
  GlobalSolution misregistered = truth_;
  for (size_t i = 0; i < misregistered.rotations.size(); ++i) {
    const double radians = (i % 2 == 0 ? 0.1 : -0.1) * std::numbers::pi / 180.0;
    misregistered.rotations[i] = Normalize(
        Multiply(FromAxisAngle(Vec3{0.0, 1.0, 0.0}, radians), misregistered.rotations[i]));
  }
  const Comparison drawn = Compose(misregistered);
  EXPECT_EQ(drawn.equatorCovered, 1.0) << "the premise: the same sphere is drawn";
  EXPECT_GT(drawn.meanError, kMeanErrorBound);
}

class RegisteredComposition : public CompositionAccuracy,
                              public ::testing::WithParamInterface<FeatureDetector> {};

// The ring registered from its own pixels and solved by `Refine`, as a phone would hand it over:
// pairs and priors three degrees out, and a lens 5% long — the order the page's assumed field of
// view can be out by on a device's first capture, before it has kept a lens (ADR 0067; no browser
// reports one). That is the registration table's `AFocalLengthOutIsFittedFromTheRing` at 1.05,
// composed.
//
// The solve faces wherever the priors agree, 0.26 degrees from the truth, and a common turn moves
// the whole preview against the reference, so the gauge comes off first: the rotation that best
// carries the solve onto the truth, applied to every frame, which leaves what registration got
// wrong relative to itself.
TEST_P(RegisteredComposition, ARegisteredRingComposesToThePhotographOnceItsGaugeIsRemoved) {
  Intrinsics guess = truth_.intrinsics;
  guess.fx *= 1.05;
  guess.fy *= 1.05;

  FeatureRegistrationEngine registration{store_, GetParam()};
  std::vector<FeatureSet> sets;
  for (const FrameRef& frame : frames_) {
    const Result<FeatureSet> features = registration.ExtractFeatures(frame);
    ASSERT_TRUE(features.ok()) << features.status.detail;
    // A count of zero allocated nothing, and the store would refuse to forget its default handles.
    if (features.value.count > 0) {
      owned_.push_back(features.value.keypoints);
      owned_.push_back(features.value.descriptors);
    }
    sets.push_back(features.value);
  }
  std::vector<PairwiseResult> pairs;
  for (size_t a = 0; a < sets.size(); ++a) {
    const size_t b = (a + 1) % sets.size();
    const Result<PairwiseResult> pair = registration.EstimatePairwise(
        sets[a], sets[b], test::PairPriorThreeDegreesOut(truth_.rotations[a], truth_.rotations[b]),
        guess);
    ASSERT_TRUE(pair.ok()) << "pair " << a << "-" << b << ": " << pair.status.detail;
    pairs.push_back(pair.value);
  }
  const Result<GlobalSolution> solved = registration.Refine(
      pairs, test::FramePriorsThreeDegreesOut(truth_.frames, truth_.rotations), guess);
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  ASSERT_EQ(solved.value.frames, truth_.frames);
  EXPECT_EQ(solved.value.edgesUsed, kFrames) << "not every pair was accepted";
  EXPECT_TRUE(solved.value.lensFitted);

  const test::GaugeAlignment gauge =
      test::BestGaugeAlignment(solved.value.rotations, truth_.rotations);
  ASSERT_TRUE(gauge.valid && gauge.isUnique);
  // The premise below that leaving the gauge on fails the bound is a premise only while there is a
  // gauge to leave on: priors that happened to agree on the truth would face the solve there, and
  // the premise would then fail for a reason that is not the compositor's.
  const double gaugeDeg = AngleBetween(gauge.rotation, Quat{}) * 180.0 / std::numbers::pi;
  ASSERT_GT(gaugeDeg, 0.15) << "the priors agree on the truth, so there is no gauge to take off";
  GlobalSolution aligned = solved.value;
  for (Quat& rotation : aligned.rotations) rotation = Normalize(Multiply(gauge.rotation, rotation));

  const Comparison drawn = Compose(aligned);
  // The two premises, each a thing the preview needed from the solve and got: the gauge taken off,
  // and the lens it fitted rather than the one it was handed.
  const Comparison facingThePriors = Compose(solved.value);
  GlobalSolution underTheGuess = aligned;
  underTheGuess.intrinsics = guess;
  const Comparison guessed = Compose(underTheGuess);

  const test::RotationScore score = test::ScoreRotations(solved.value.rotations, truth_.rotations);
  // One line a detector: the gate counts them against the instantiations, as it does `[accuracy]`.
  std::printf("[registered] detector=%d fx=%+.4f%% gauge=%.4f deg median=%.4f max=%.4f mean=%.3f "
              "p99=%d unaligned=%.3f guessed=%.3f\n",
              static_cast<int>(GetParam()),
              100.0 * (solved.value.intrinsics.fx / truth_.intrinsics.fx - 1.0),
              gaugeDeg, score.medianDeg,
              score.maxDeg, drawn.meanError, drawn.p99Error, facingThePriors.meanError,
              guessed.meanError);

  // Today's run: 1.547, 1.378 and 1.215 (ORB, AKAZE, SIFT) against the truth's 1.193, p99 14, 13
  // and 11; about 5 with the gauge left on, and 14.2 to 14.3 under the guess. The truth's bound
  // rather than one per detector: the rotations are bounded in degrees in
  // `registration_accuracy_test.cpp`, and what this adds is that nothing between the solve and the
  // preview loses what they got right. Written also in `docs/06-roadmap.md` and `CLAUDE.md`, which
  // move with these.
  EXPECT_EQ(drawn.equatorCovered, 1.0);
  EXPECT_LE(drawn.meanError, kMeanErrorBound);
  EXPECT_LE(drawn.p99Error, kP99ErrorBound);
  EXPECT_GT(facingThePriors.meanError, kMeanErrorBound);
  EXPECT_GT(guessed.meanError, kMeanErrorBound);
}

INSTANTIATE_TEST_SUITE_P(EveryDetector, RegisteredComposition,
                         ::testing::ValuesIn(kAllFeatureDetectors));

}  // namespace
}  // namespace sphanorama
