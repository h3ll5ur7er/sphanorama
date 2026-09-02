// The capture session's sequence, from docs/03 UC-1.
//
// What is under test is *when* the manager asks each collaborator, never what they answer — the
// answers belong to engines behind contracts. Driving it with fakes is only cheap because the
// camera and sensors are resource-access contracts rather than browser calls, which is the whole
// argument for that layer.
#include <gtest/gtest.h>

#include <memory>

#include "engines/coverage_planner_engine/null_coverage_planner_engine.h"
#include "engines/frame_quality_engine/null_frame_quality_engine.h"
#include "engines/pose_engine/null_pose_engine.h"
#include "managers/capture_session_manager/capture_session_manager.h"
#include "support/fake_camera_access.h"
#include "support/fake_frame_store_access.h"
#include "support/fake_motion_sensor_access.h"
#include "support/fake_project_store_access.h"

namespace sphanorama {
namespace {

constexpr ProjectId kProject{1};

class CaptureSession : public ::testing::Test {
 protected:
  void SetUp() override {
    store = std::make_shared<FakeFrameStoreAccess>(1 << 22);
    camera = std::make_unique<FakeCameraAccess>(store);
    sensor = std::make_unique<FakeMotionSensorAccess>();
    projects = std::make_unique<FakeProjectStoreAccess>();
    // A session belongs to a project that already exists. Creating one here rather than letting
    // Begin conjure it is the point of the test below.
    (void)projects->WriteDocument(kProject, "title", "test project");

    manager = std::make_unique<CaptureSessionManager>(
        planner, pose, quality, *camera, *sensor, *store, *projects);
  }

  CapturePlanSpec Spec() {
    CapturePlanSpec spec;
    spec.acceptanceConeDeg = 5.0;
    return spec;
  }

  SessionId Begin() {
    auto begun = manager->Begin(kProject, Spec());
    EXPECT_TRUE(begun.ok()) << begun.status.detail;
    return begun.value;
  }

  NodeId FirstNode() { return manager->GetPlan().value.nodes.front().id; }

  NullCoveragePlannerEngine planner;
  NullPoseEngine pose;
  NullFrameQualityEngine quality;
  std::shared_ptr<FakeFrameStoreAccess> store;
  std::unique_ptr<FakeCameraAccess> camera;
  std::unique_ptr<FakeMotionSensorAccess> sensor;
  std::unique_ptr<FakeProjectStoreAccess> projects;
  std::unique_ptr<CaptureSessionManager> manager;
};

TEST_F(CaptureSession, BeginOnAProjectThatDoesNotExistIsRefused) {
  // End writes a session document through the project store, and the store creates storage on
  // demand — so a session begun against an arbitrary id leaves a project behind with no title,
  // which then shows up in the user's list as a blank row nobody made.
  FakeProjectStoreAccess empty;
  CaptureSessionManager orphan(planner, pose, quality, *camera, *sensor, *store, empty);
  auto begun = orphan.Begin(ProjectId{404}, Spec());
  EXPECT_EQ(begun.status.code, StatusCode::NotFound);
  // Refused before the camera is touched: a permission prompt for a session that cannot start is
  // the worst possible order to do these in.
  EXPECT_FALSE(camera->IsOpen());
}

TEST_F(CaptureSession, CandidatesForACellOutsideThePlanIsRefused) {
  // CaptureCell and RequestRetake both answer NotFound here. Returning an empty success instead
  // makes a typo'd cell indistinguishable from a real one nobody has captured yet.
  Begin();
  EXPECT_EQ(manager->Candidates(NodeId{9999}).status.code, StatusCode::NotFound);
}

TEST_F(CaptureSession, BeginProducesASessionAndAPlan) {
  const SessionId session = Begin();
  EXPECT_TRUE(session.valid());
  auto plan = manager->GetPlan();
  ASSERT_TRUE(plan.ok());
  EXPECT_FALSE(plan.value.nodes.empty());
}

TEST_F(CaptureSession, BeginOpensTheCameraSoAPreviewCanStart) {
  Begin();
  // Opening is the manager's job, not the client's: the client calls a use case, and everything
  // that use case needs to happen is sequenced here.
  EXPECT_TRUE(camera->CaptureBurst(BurstSpec{}).ok());
}

TEST_F(CaptureSession, RejectsANonsensePlanSpecRatherThanStartingASession) {
  CapturePlanSpec spec;
  spec.acceptanceConeDeg = 0.0;
  EXPECT_EQ(manager->Begin(kProject, spec).status.code, StatusCode::InvalidArgument);
}

TEST_F(CaptureSession, WorkBeforeBeginIsRefused) {
  EXPECT_EQ(manager->GetPlan().status.code, StatusCode::FailedPrecondition);
  EXPECT_EQ(manager->OnMotion({}).status.code, StatusCode::FailedPrecondition);
  EXPECT_EQ(manager->Coverage().status.code, StatusCode::FailedPrecondition);
  EXPECT_EQ(manager->CaptureCell(NodeId{1}, BurstSpec{}).status.code,
            StatusCode::FailedPrecondition);
}

TEST_F(CaptureSession, OnMotionReturnsGuidanceForACell) {
  Begin();
  std::vector<ImuSample> samples(4);
  auto guidance = manager->OnMotion(samples);
  ASSERT_TRUE(guidance.ok()) << guidance.status.detail;
  EXPECT_TRUE(guidance.value.targetNode.valid());
  EXPECT_GE(guidance.value.angularErrorDeg, 0.0);
}

TEST_F(CaptureSession, OnMotionWithNoSamplesIsNotAnError) {
  // The capture loop calls this every frame whether or not the sensor produced anything, and a
  // failure on an empty batch would make the common case the error path.
  Begin();
  EXPECT_TRUE(manager->OnMotion({}).ok());
}

TEST_F(CaptureSession, CaptureCellFillsTheCellWithABurst) {
  Begin();
  BurstSpec burst;
  burst.frameCount = 4;
  auto candidates = manager->CaptureCell(FirstNode(), burst);
  ASSERT_TRUE(candidates.ok()) << candidates.status.detail;
  EXPECT_EQ(candidates.value.size(), 4u);
}

TEST_F(CaptureSession, EveryCandidateCarriesItsCellAndAScore) {
  Begin();
  BurstSpec burst;
  burst.frameCount = 3;
  const NodeId node = FirstNode();
  auto candidates = manager->CaptureCell(node, burst);
  ASSERT_TRUE(candidates.ok());
  for (const auto& candidate : candidates.value) {
    EXPECT_EQ(candidate.node.value, node.value);
    EXPECT_TRUE(candidate.id.valid());
    EXPECT_TRUE(candidate.frame.id.valid());
  }
}

TEST_F(CaptureSession, LocksExposureBeforeFiringABurst) {
  // Every frame in a burst must share an exposure, or selecting between them compares brightness
  // rather than sharpness — and the blend would band across the cell.
  Begin();
  BurstSpec burst;
  burst.frameCount = 2;
  burst.lockExposure = true;
  ASSERT_TRUE(manager->CaptureCell(FirstNode(), burst).ok());
  EXPECT_TRUE(camera->ExposureLocked());
}

TEST_F(CaptureSession, CapturingAnUnknownCellIsRefused) {
  Begin();
  EXPECT_EQ(manager->CaptureCell(NodeId{9999}, BurstSpec{}).status.code, StatusCode::NotFound);
}

TEST_F(CaptureSession, CandidatesAccumulateAcrossBursts) {
  // A retake adds evidence rather than replacing it, so the pool has to grow.
  Begin();
  BurstSpec burst;
  burst.frameCount = 2;
  const NodeId node = FirstNode();
  ASSERT_TRUE(manager->CaptureCell(node, burst).ok());
  ASSERT_TRUE(manager->CaptureCell(node, burst).ok());
  auto candidates = manager->Candidates(node);
  ASSERT_TRUE(candidates.ok());
  EXPECT_EQ(candidates.value.size(), 4u);
}

TEST_F(CaptureSession, CoverageReportsTheCellAsAHoleUntilItIsShot) {
  Begin();
  auto before = manager->Coverage();
  ASSERT_TRUE(before.ok());
  EXPECT_EQ(before.value.nodesSatisfied, 0);
  EXPECT_FALSE(before.value.holes.empty());

  BurstSpec burst;
  burst.frameCount = 1;
  ASSERT_TRUE(manager->CaptureCell(FirstNode(), burst).ok());

  auto after = manager->Coverage();
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after.value.nodesSatisfied, 1);
  EXPECT_TRUE(after.value.holes.empty());
}

TEST_F(CaptureSession, RetakeKeepsExistingEvidenceByDefault) {
  Begin();
  BurstSpec burst;
  burst.frameCount = 2;
  const NodeId node = FirstNode();
  ASSERT_TRUE(manager->CaptureCell(node, burst).ok());
  ASSERT_TRUE(manager->RequestRetake(node, /*replace=*/false).ok());
  EXPECT_EQ(manager->Candidates(node).value.size(), 2u);
}

TEST_F(CaptureSession, RetakeCanDiscardWhenAskedTo) {
  // Both behaviours are real: a ghosted cell may be worth re-shooting from scratch, while a
  // blurred one is worth adding to.
  Begin();
  BurstSpec burst;
  burst.frameCount = 2;
  const NodeId node = FirstNode();
  ASSERT_TRUE(manager->CaptureCell(node, burst).ok());
  ASSERT_TRUE(manager->RequestRetake(node, /*replace=*/true).ok());
  EXPECT_TRUE(manager->Candidates(node).value.empty());
}

TEST_F(CaptureSession, RetakingAnUnknownCellIsRefused) {
  Begin();
  EXPECT_EQ(manager->RequestRetake(NodeId{9999}, false).code, StatusCode::NotFound);
}

TEST_F(CaptureSession, DiscardedFramesAreReleasedFromTheStore) {
  // 48 cells of bursts is roughly 15 GB. A replace-retake that leaked its old frames would run
  // the device out of memory inside one session.
  Begin();
  BurstSpec burst;
  burst.frameCount = 3;
  const NodeId node = FirstNode();
  ASSERT_TRUE(manager->CaptureCell(node, burst).ok());
  const int64_t held = store->Budget().value.heapUsedBytes;
  ASSERT_GT(held, 0);

  ASSERT_TRUE(manager->RequestRetake(node, /*replace=*/true).ok());
  EXPECT_LT(store->Budget().value.heapUsedBytes, held);
}

TEST_F(CaptureSession, EndPersistsTheSessionSoItCanBeResumed) {
  Begin();
  BurstSpec burst;
  burst.frameCount = 1;
  ASSERT_TRUE(manager->CaptureCell(FirstNode(), burst).ok());
  ASSERT_TRUE(manager->End().ok());
  EXPECT_GT(projects->WriteCount(), 0);
}

TEST_F(CaptureSession, EndClosesTheCameraSoTheIndicatorGoesOut) {
  // A session that leaves the camera running reads to the user as the app watching them.
  Begin();
  ASSERT_TRUE(manager->End().ok());
  EXPECT_EQ(camera->CaptureBurst(BurstSpec{}).status.code, StatusCode::FailedPrecondition);
}

TEST_F(CaptureSession, WorkAfterEndIsRefused) {
  Begin();
  ASSERT_TRUE(manager->End().ok());
  EXPECT_EQ(manager->OnMotion({}).status.code, StatusCode::FailedPrecondition);
}

TEST_F(CaptureSession, ASessionCanBeBegunAgainAfterEnding) {
  const SessionId first = Begin();
  ASSERT_TRUE(manager->End().ok());
  const SessionId second = Begin();
  EXPECT_NE(first.value, second.value);
  EXPECT_TRUE(manager->Candidates(FirstNode()).value.empty());
}

}  // namespace
}  // namespace sphanorama
