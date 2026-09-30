/**
 * **Does a build of a capture draw the world the capture was taken of?** The build manager run over
 * the real registration and composition engines, from a session document naming a rendered ring,
 * compared with the photograph the ring was rendered from (ADR 0070).
 *
 * The ring's poses are the truth turned three degrees either way, alternately about the camera's
 * horizontal axis. Each frame alone is three degrees out, which composes far past the bound — the
 * test asserts that too, so it is the pairs that place the frames and not the poses. And the turns
 * cancel round the ring, so the solve faces the truth without a gauge to take off: this compares
 * the panorama the manager answers, exactly as a client would read it.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <numbers>
#include <vector>

#include "engines/composition_engine/nearest_centre_composition_engine.h"
#include "engines/registration_engine/feature_registration_engine.h"
#include "managers/panorama_build_manager/panorama_build_manager.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "support/fake_project_store_access.h"
#include "support/rendered_dataset.h"
#include "support/synthetic_dataset.h"
#include "utilities/camera_model.h"
#include "utilities/quaternion.h"
#include "utilities/session_document.h"

namespace sphanorama {
namespace {

using test::Rendered;
using test::World;

constexpr int kFrames = 12;
constexpr int32_t kPreviewWidth = 1024;
constexpr ProjectId kProject{1};
// The bound `composition_accuracy_test.cpp` holds a registered ring to, and for the same reason:
// the truth composes at 1.19 and a tenth of a degree of error at 2.84.
constexpr double kMeanErrorBound = 2.0;

// Mean absolute colour error over the covered pixels, and the fraction of the equator covered.
struct Comparison {
  double meanError = 0.0;
  double equatorCovered = 0.0;
};

class PanoramaBuildAccuracy : public ::testing::TestWithParam<FeatureDetector> {
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
    ASSERT_EQ(dataset.value.frames.size(), static_cast<size_t>(kFrames));
    lens_ = dataset.value.lens;
    for (const SyntheticFrame& frame : dataset.value.frames) {
      owned_.push_back(frame.frame);
      frames_.push_back(frame.frame);
      truth_.push_back(frame.trueRotation);
    }
    Result<FrameRef> reference =
        LoadSyntheticReference(store_, rendered_->path(), kPreviewWidth, kPreviewWidth / 2);
    ASSERT_TRUE(reference.ok()) << reference.status.detail;
    owned_.push_back(reference.value);
    reference_ = reference.value;
  }

  void TearDown() override {
    for (const FrameRef& frame : owned_) {
      const Status forgotten = store_.Forget(frame);
      EXPECT_TRUE(forgotten.ok()) << "frame " << frame.id.value << ": " << forgotten.detail;
    }
    Result<FrameStoreBudget> budget = store_.Budget();
    ASSERT_TRUE(budget.ok());
    EXPECT_EQ(budget.value.heapUsedBytes, 0);
  }

  // The poses a phone would have reported: the truth, turned three degrees about the camera's
  // horizontal axis, one way on even frames and the other on odd.
  std::vector<Quat> Poses() const {
    std::vector<Quat> poses;
    for (size_t i = 0; i < truth_.size(); ++i) {
      const double turn = (i % 2 == 0 ? 3.0 : -3.0) * std::numbers::pi / 180.0;
      poses.push_back(Normalize(Multiply(truth_[i], FromAxisAngle(Vec3{1, 0, 0}, turn))));
    }
    return poses;
  }

  void WriteCapture(const std::vector<Quat>& poses) {
    ASSERT_TRUE(projects_.WriteDocument(kProject, "title", "a hangar").ok());
    auto generation = store_.TierGeneration();
    ASSERT_TRUE(generation.ok());
    SessionDocument document;
    document.session = 1;
    document.generation = generation.value;
    document.spec.horizontalFovDeg = HorizontalFovDeg(lens_);
    document.spec.verticalFovDeg = VerticalFovDeg(lens_);
    document.lens.width = lens_.width;
    document.lens.height = lens_.height;
    for (size_t i = 0; i < frames_.size(); ++i) {
      Candidate candidate;
      candidate.id = CandidateId{i + 1};
      candidate.node = NodeId{i + 1};
      candidate.frame = frames_[i];
      candidate.pose.orientation = poses[i];
      candidate.pose.confidence = 1.0;
      document.candidates.push_back(candidate);
    }
    document.nextCandidate = frames_.size() + 1;
    ASSERT_TRUE(projects_.WriteDocument(kProject, kSessionDocumentKey,
                                        EncodeSessionDocument(document)).ok());
  }

  Comparison Compare(const FrameRef& drawn) {
    Comparison result;
    EXPECT_EQ(drawn.width, reference_.width);
    EXPECT_EQ(drawn.height, reference_.height);
    if (drawn.width != reference_.width || drawn.height != reference_.height) return result;
    Result<std::span<uint8_t>> a = store_.Pin(drawn);
    Result<std::span<uint8_t>> b = store_.Pin(reference_);
    EXPECT_TRUE(a.ok() && b.ok());
    if (a.ok() && b.ok()) {
      double sum = 0.0;
      int64_t components = 0;
      int64_t equator = 0;
      for (int32_t y = 0; y < drawn.height; ++y) {
        for (int32_t x = 0; x < drawn.width; ++x) {
          const uint8_t* p = a.value.data() + static_cast<size_t>(y) * drawn.stride + x * 4;
          const uint8_t* r = b.value.data() + static_cast<size_t>(y) * reference_.stride + x * 4;
          if (p[3] != 255) continue;
          if (y == drawn.height / 2) ++equator;
          for (int c = 0; c < 3; ++c) sum += std::abs(p[c] - r[c]);
          components += 3;
        }
      }
      result.meanError = components > 0 ? sum / static_cast<double>(components) : 0.0;
      result.equatorCovered = static_cast<double>(equator) / drawn.width;
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
  FakeProjectStoreAccess projects_;
  NearestCentreCompositionEngine composition_{store_};
  Intrinsics lens_;
  std::vector<FrameRef> owned_;
  std::vector<FrameRef> frames_;
  std::vector<Quat> truth_;
  FrameRef reference_;
};

std::unique_ptr<Rendered> PanoramaBuildAccuracy::rendered_;

TEST_P(PanoramaBuildAccuracy, ABuiltCaptureDrawsThePhotograph) {
  const std::vector<Quat> poses = Poses();
  WriteCapture(poses);

  // The premise: composed where the poses say, the frames are far past the bound, so a build that
  // passed it placed them from their pixels.
  GlobalSolution posed;
  posed.intrinsics = lens_;
  for (size_t i = 0; i < frames_.size(); ++i) {
    posed.frames.push_back(frames_[i].id);
    posed.rotations.push_back(poses[i]);
  }
  Result<FrameRef> unregistered = composition_.RenderPreview(posed, frames_, {}, kPreviewWidth);
  ASSERT_TRUE(unregistered.ok()) << unregistered.status.detail;
  owned_.push_back(unregistered.value);
  const Comparison asPosed = Compare(unregistered.value);

  FeatureRegistrationEngine registration{store_, GetParam()};
  PanoramaBuildManager manager{registration, composition_, store_, projects_};
  BuildSpec spec;
  spec.outputWidth = kPreviewWidth;
  auto build = manager.Start(kProject, spec);
  ASSERT_TRUE(build.ok()) << build.status.detail;
  BuildProgress progress;
  int polls = 0;
  while (progress.stage != BuildStage::Complete && progress.stage != BuildStage::Failed) {
    auto polled = manager.Poll(build.value);
    ASSERT_TRUE(polled.ok()) << polled.status.detail;
    progress = polled.value;
    ASSERT_LT(++polls, 100);
  }
  ASSERT_EQ(progress.stage, BuildStage::Complete) << progress.failure.detail;
  // Twelve extractions, the ring's twelve pairs, a solve and a preview.
  EXPECT_EQ(polls, 26);

  auto panorama = manager.Panorama(build.value);
  ASSERT_TRUE(panorama.ok()) << panorama.status.detail;
  const Comparison built = Compare(panorama.value.preview);
  std::printf("[built] detector=%d mean=%.3f posed=%.3f\n", static_cast<int>(GetParam()),
              built.meanError, asPosed.meanError);
  EXPECT_EQ(built.equatorCovered, 1.0);
  EXPECT_LE(built.meanError, kMeanErrorBound);
  EXPECT_GT(asPosed.meanError, kMeanErrorBound);

  // The panorama is the manager's, so it goes back through the manager.
  EXPECT_TRUE(manager.Cancel(build.value).ok());
}

INSTANTIATE_TEST_SUITE_P(EveryDetector, PanoramaBuildAccuracy,
                         ::testing::ValuesIn(kAllFeatureDetectors));

}  // namespace
}  // namespace sphanorama
