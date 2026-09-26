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
 * rotation moves the whole preview against the reference — so an *estimated* solution has its
 * gauge taken off before it is composed, and that is the next increment rather than this one.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <vector>

#include "engines/composition_engine/nearest_centre_composition_engine.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "support/rendered_dataset.h"
#include "support/synthetic_dataset.h"
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
    for (const FrameRef& frame : owned_) EXPECT_TRUE(store_.Forget(frame).ok());
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
    EXPECT_EQ(drawn.width, reference_.width);
    EXPECT_EQ(drawn.height, reference_.height);
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

}  // namespace
}  // namespace sphanorama
