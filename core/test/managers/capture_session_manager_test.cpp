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
#include "engines/coverage_planner_engine/rings_coverage_planner_engine.h"
#include "engines/pose_engine/null_pose_engine.h"
#include "managers/capture_session_manager/capture_session_manager.h"
#include "support/fake_camera_access.h"
#include "support/fake_frame_store_access.h"
#include "support/fake_motion_sensor_access.h"
#include "support/fake_project_store_access.h"
#include "utilities/clock.h"

namespace sphanorama {
namespace {

constexpr ProjectId kProject{1};

// What the capture loop does: arm a cell, then keep ticking. A burst takes one frame per tick and
// no faster than its interval (ADR 0018), so the clock has to move for it to finish — and a helper
// that ticked forever would turn a burst that never completes into a hung test rather than a
// failing one.
Status FireBurstOn(ICaptureSessionManager& manager, ManualClock& clock, NodeId node,
                   const BurstSpec& burst) {
  if (auto armed = manager.ArmBurst(node, burst); !armed.ok()) return armed;
  for (int32_t tick = 0; tick < burst.frameCount * 4 + 8; ++tick) {
    auto guidance = manager.OnMotion({});
    if (!guidance.ok()) return guidance.status;
    if (guidance.value.action == GuidanceAction::CellDone) return Status::Ok();
    clock.AdvanceMs(burst.intervalMs);
  }
  return Fail(StatusCode::Internal, "test", "the burst never finished");
}

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
        planner, pose, quality, *camera, *sensor, *store, *projects, clock);
  }

  Status FireBurst(NodeId node, const BurstSpec& burst) {
    return FireBurstOn(*manager, clock, node, burst);
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
  ManualClock clock;
  std::unique_ptr<CaptureSessionManager> manager;
};

TEST_F(CaptureSession, BeginOnAProjectThatDoesNotExistIsRefused) {
  // End writes a session document through the project store, and the store creates storage on
  // demand — so a session begun against an arbitrary id leaves a project behind with no title,
  // which then shows up in the user's list as a blank row nobody made.
  FakeProjectStoreAccess empty;
  CaptureSessionManager orphan(planner, pose, quality, *camera, *sensor, *store, empty, clock);
  auto begun = orphan.Begin(ProjectId{404}, Spec());
  EXPECT_EQ(begun.status.code, StatusCode::NotFound);
  // Refused before the camera is touched: a permission prompt for a session that cannot start is
  // the worst possible order to do these in.
  EXPECT_FALSE(camera->IsOpen());
}

TEST_F(CaptureSession, OnMotionPullsFromTheSensorWhenTheClientHasNothingToPush) {
  // Two ways in exist because two kinds of client exist: one that already holds samples (the
  // browser drains them in JavaScript) and one that does not (the bench replays a log through
  // the port). A manager that only accepted pushed samples would make IMotionSensorAccess::Drain
  // dead code on every platform, which is how a port stops being substitutable for its contract.
  Begin();
  sensor->EnqueueSpin(4, 10'000'000, 0.5);

  auto guidance = manager->OnMotion({});
  ASSERT_TRUE(guidance.ok()) << guidance.status.detail;
  // The samples were consumed, not left for the next call to read again.
  ImuSample scratch[8];
  auto remaining = sensor->Drain(std::span<ImuSample>(scratch, 8));
  ASSERT_TRUE(remaining.ok());
  EXPECT_EQ(remaining.value, 0);
}

TEST_F(CaptureSession, PushedSamplesAreNotTakenTwice) {
  // A client that pushes must not also get the port drained underneath it, or every sample would
  // be integrated once from each path and the pose would advance at double rate.
  Begin();
  sensor->EnqueueSpin(4, 10'000'000, 0.5);

  ImuSample pushed;
  pushed.timestampNs = 1'000'000;
  pushed.hasOrientation = true;
  ASSERT_TRUE(manager->OnMotion(std::span<const ImuSample>(&pushed, 1)).ok());

  ImuSample scratch[8];
  auto remaining = sensor->Drain(std::span<ImuSample>(scratch, 8));
  ASSERT_TRUE(remaining.ok());
  EXPECT_EQ(remaining.value, 4);
}

TEST_F(CaptureSession, ASecondBeginWithoutAnEndIsRefused) {
  // Silently replacing the session dropped the candidate map on the floor: those frames stayed
  // pinned in the store with nothing left holding their handles, so a full sphere of bursts
  // leaked on a double-tap of the enable button. It also restarted the camera and sensor
  // underneath a session the caller still believed in.
  const SessionId first = Begin();
  auto again = manager->Begin(kProject, Spec());
  EXPECT_EQ(again.status.code, StatusCode::FailedPrecondition);

  // And the first session is untouched, not half-replaced.
  auto plan = manager->GetPlan();
  ASSERT_TRUE(plan.ok());
  EXPECT_FALSE(plan.value.nodes.empty());
  ASSERT_TRUE(manager->End().ok());
  EXPECT_GT(first.value, 0u);
}

TEST_F(CaptureSession, BeginningAgainAfterEndIsFine) {
  Begin();
  ASSERT_TRUE(manager->End().ok());
  EXPECT_TRUE(manager->Begin(kProject, Spec()).ok());
}

TEST_F(CaptureSession, CandidatesForACellOutsideThePlanIsRefused) {
  // ArmBurst and RequestRetake both answer NotFound here. Returning an empty success instead
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
  // that use case needs to happen is sequenced here. Preview too — a burst is peeks now, and a
  // peek before StartPreview is a FailedPrecondition.
  EXPECT_TRUE(camera->PeekPreviewFrame().ok());
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
  EXPECT_EQ(manager->ArmBurst(NodeId{1}, BurstSpec{}).code, StatusCode::FailedPrecondition);
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

// A quality engine that refuses, so the manager's handling of that refusal can be tested. The
// null one succeeds with a default score, which is why nothing caught this.
class RefusingFrameQualityEngine final : public IFrameQualityEngine {
 public:
  Result<QualityScore> Score(const FrameRef&, const PoseSample&, const NodeContext&) override {
    return Err<QualityScore>(StatusCode::ComputeUnavailable, "test", "cannot score");
  }
  Result<std::vector<CandidateId>> Rank(std::span<const Candidate>,
                                        const SelectionPolicy&) override {
    return Err<std::vector<CandidateId>>(StatusCode::ComputeUnavailable, "test", "cannot rank");
  }
};

// Scores fine, cannot rank. Ranking is what turns a burst into a choice, so a set nobody could
// rank is not a captured cell.
class UnrankableFrameQualityEngine final : public IFrameQualityEngine {
 public:
  Result<QualityScore> Score(const FrameRef&, const PoseSample&, const NodeContext&) override {
    return Ok(QualityScore{});
  }
  Result<std::vector<CandidateId>> Rank(std::span<const Candidate>,
                                        const SelectionPolicy&) override {
    return Err<std::vector<CandidateId>>(StatusCode::ComputeUnavailable, "test", "cannot rank");
  }
};

TEST_F(CaptureSession, ABurstThatCannotBeRankedIsRolledBackToo) {
  // The scoring path rolls back; ranking was still discarded with a (void) cast, so a cell could
  // end up holding candidates the selection engine had failed on while the burst reported
  // success. Picking the best of a burst is the point; an unrankable burst is not a capture.
  UnrankableFrameQualityEngine unrankable;
  CaptureSessionManager manager(planner, pose, unrankable, *camera, *sensor, *store, *projects, clock);
  ASSERT_TRUE(manager.Begin(kProject, Spec()).ok());
  const NodeId node = manager.GetPlan().value.nodes.front().id;

  const int64_t before = store->Budget().value.heapUsedBytes;
  BurstSpec burst;
  burst.frameCount = 3;
  EXPECT_EQ(FireBurstOn(manager, clock, node, burst).code, StatusCode::ComputeUnavailable);
  EXPECT_TRUE(manager.Candidates(node).value.empty());
  EXPECT_EQ(store->Budget().value.heapUsedBytes, before);
}

// Refuses to plan, so the manager's cleanup after a planning failure can be tested. The null
// planner always succeeds, which is why nothing reached this path.
class RefusingCoveragePlannerEngine final : public ICoveragePlannerEngine {
 public:
  Result<CapturePlan> Plan(const CapturePlanSpec&, const Intrinsics&) override {
    return Err<CapturePlan>(StatusCode::Unsupported, "test", "cannot plan");
  }
  Result<CaptureGuidance> Locate(const Quat&, const CapturePlan&) override {
    return Err<CaptureGuidance>(StatusCode::Unsupported, "test", "cannot locate");
  }
  Result<CoverageState> Evaluate(const CapturePlan&, std::span<const Candidate>) override {
    return Err<CoverageState>(StatusCode::Unsupported, "test", "cannot evaluate");
  }
  Result<std::vector<NodeId>> SuggestRetakes(const CapturePlan&, const CoverageState&,
                                             const GhostReport&) override {
    return Err<std::vector<NodeId>>(StatusCode::Unsupported, "test", "cannot suggest");
  }
};

TEST_F(CaptureSession, AFailedPlanClosesTheCameraItOpened) {
  // The lens has to be read before the plan can be made, so a planning failure happens with the
  // camera already open — and returning without closing leaves the indicator lit for a session
  // that never started. Rejecting an unsupported strategy and a nonsense field of view, both
  // added recently, are exactly what make this reachable.
  RefusingCoveragePlannerEngine refusing;
  CaptureSessionManager manager(refusing, pose, quality, *camera, *sensor, *store, *projects, clock);

  EXPECT_EQ(manager.Begin(kProject, Spec()).status.code, StatusCode::Unsupported);
  EXPECT_FALSE(camera->IsOpen());
  // And it is left genuinely idle, not half-begun.
  EXPECT_EQ(manager.GetPlan().status.code, StatusCode::FailedPrecondition);
}

TEST_F(CaptureSession, AFailedScoreDoesNotBecomeACandidateWithAZeroScore) {
  // A zero QualityScore is what a genuinely terrible frame gets. Using it for "the scorer broke"
  // makes the two indistinguishable — and the cell then counts toward coverage, so the sphere
  // reports a cell complete that nothing ever judged. Selection would later pick a "best" frame
  // from a set where every score is a placeholder.
  RefusingFrameQualityEngine refusing;
  CaptureSessionManager manager(planner, pose, refusing, *camera, *sensor, *store, *projects, clock);
  ASSERT_TRUE(manager.Begin(kProject, Spec()).ok());
  const NodeId node = manager.GetPlan().value.nodes.front().id;

  BurstSpec burst;
  burst.frameCount = 3;
  EXPECT_EQ(FireBurstOn(manager, clock, node, burst).code, StatusCode::ComputeUnavailable);

  // Nothing half-added: the cell is as it was.
  auto candidates = manager.Candidates(node);
  ASSERT_TRUE(candidates.ok());
  EXPECT_TRUE(candidates.value.empty());
}

TEST_F(CaptureSession, AFailedBurstReleasesTheFramesItAlreadyTook) {
  // The frames were allocated by this call, so this call owns them. Returning without releasing
  // leaves a burst pinned in the store with nothing holding the handles — and a full sphere of
  // those is the memory ceiling this whole design is arranged around.
  RefusingFrameQualityEngine refusing;
  CaptureSessionManager manager(planner, pose, refusing, *camera, *sensor, *store, *projects, clock);
  ASSERT_TRUE(manager.Begin(kProject, Spec()).ok());
  const NodeId node = manager.GetPlan().value.nodes.front().id;

  const int64_t before = store->Budget().value.heapUsedBytes;
  BurstSpec burst;
  burst.frameCount = 3;
  ASSERT_FALSE(FireBurstOn(manager, clock, node, burst).ok());
  EXPECT_EQ(store->Budget().value.heapUsedBytes, before);
}

TEST_F(CaptureSession, AnOfferedFrameThatCannotBeScoredIsRefusedRatherThanAccepted) {
  // Same reasoning, except the frame belongs to the caller — so it is reported, not forgotten.
  RefusingFrameQualityEngine refusing;
  CaptureSessionManager manager(planner, pose, refusing, *camera, *sensor, *store, *projects, clock);
  ASSERT_TRUE(manager.Begin(kProject, Spec()).ok());
  const NodeId node = manager.GetPlan().value.nodes.front().id;

  auto frame = store->Allocate(4, 4, PixelFormat::RGBA8);
  ASSERT_TRUE(frame.ok());
  auto verdict = manager.OfferFrame(node, frame.value, PoseSample{});
  EXPECT_EQ(verdict.status.code, StatusCode::ComputeUnavailable);
  EXPECT_TRUE(manager.Candidates(node).value.empty());
  // Not forgotten: the caller passed it in and still owns it.
  EXPECT_TRUE(store->ResidencyOf(frame.value).ok());
}

TEST_F(CaptureSession, AnArmedBurstFillsTheCellOverTheTicksThatFollow) {
  Begin();
  BurstSpec burst;
  burst.frameCount = 4;
  const NodeId node = FirstNode();
  ASSERT_TRUE(manager->ArmBurst(node, burst).ok());
  // Arming fires nothing. The frames arrive on the ticks the capture loop was making anyway,
  // which is the whole of ADR 0018.
  EXPECT_TRUE(manager->Candidates(node).value.empty());

  // Ticked by hand rather than through FireBurst, so the guidance the client would render on the
  // way is asserted too: Firing until the burst is full, CellDone exactly on the tick that
  // fills it, and never CellDone twice.
  for (int32_t taken = 1; taken <= burst.frameCount; ++taken) {
    auto guidance = manager->OnMotion({});
    ASSERT_TRUE(guidance.ok()) << guidance.status.detail;
    EXPECT_EQ(guidance.value.action,
              taken == burst.frameCount ? GuidanceAction::CellDone : GuidanceAction::Firing)
        << "on frame " << taken;
    EXPECT_EQ(manager->Candidates(node).value.size(), static_cast<size_t>(taken));
    clock.AdvanceMs(burst.intervalMs);
  }
  EXPECT_EQ(manager->Candidates(node).value.size(), 4u);
}

TEST_F(CaptureSession, EveryCandidateCarriesItsCellAndAScore) {
  Begin();
  BurstSpec burst;
  burst.frameCount = 3;
  const NodeId node = FirstNode();
  ASSERT_TRUE(FireBurst(node, burst).ok());
  auto candidates = manager->Candidates(node);
  ASSERT_TRUE(candidates.ok());
  for (const auto& candidate : candidates.value) {
    EXPECT_EQ(candidate.node.value, node.value);
    EXPECT_TRUE(candidate.id.valid());
    EXPECT_TRUE(candidate.frame.id.valid());
  }
}

TEST_F(CaptureSession, LocksExposureWhileABurstIsInFlightAndReleasesItAfter) {
  // Every frame in a burst must share an exposure, or selecting between them compares brightness
  // rather than sharpness — and the blend would band across the cell. Since the burst now spans
  // ticks the lock does too (ADR 0018), which makes releasing it the other half of the same
  // requirement: a burst that ends leaving the exposure pinned is a viewfinder the user cannot
  // fix by pointing somewhere else.
  Begin();
  BurstSpec burst;
  burst.frameCount = 2;
  burst.lockExposure = true;
  ASSERT_TRUE(manager->ArmBurst(FirstNode(), burst).ok());

  ASSERT_TRUE(manager->OnMotion({}).ok());
  EXPECT_TRUE(camera->ExposureLocked()) << "held across the ticks the burst spans";

  clock.AdvanceMs(burst.intervalMs);
  ASSERT_TRUE(manager->OnMotion({}).ok());
  EXPECT_FALSE(camera->ExposureLocked()) << "and given back when the burst completes";
}

TEST_F(CaptureSession, RefusesASecondBurstWhileOneIsStillInFlight) {
  // Two at once is not something a single camera can honestly serve, and the second arm would
  // strand the first one's exposure lock and its rollback mark.
  Begin();
  BurstSpec burst;
  burst.frameCount = 3;
  const NodeId node = FirstNode();
  ASSERT_TRUE(manager->ArmBurst(node, burst).ok());
  EXPECT_EQ(manager->ArmBurst(node, burst).code, StatusCode::FailedPrecondition);

  // The refusal left the burst already in flight alone rather than disarming it on the way out:
  // it still fills on the ticks that follow.
  for (int32_t i = 0; i < burst.frameCount; ++i) {
    ASSERT_TRUE(manager->OnMotion({}).ok());
    clock.AdvanceMs(burst.intervalMs);
  }
  EXPECT_EQ(manager->Candidates(node).value.size(), 3u);
}

TEST_F(CaptureSession, RefusesABurstOfNoFrames) {
  // This used to be the camera's rule, back when a burst was one call into it. The camera has no
  // burst verb any more (ADR 0018), so the check belongs to whoever still counts frames.
  Begin();
  BurstSpec burst;
  burst.frameCount = 0;
  EXPECT_EQ(manager->ArmBurst(FirstNode(), burst).code, StatusCode::InvalidArgument);
}

TEST_F(CaptureSession, TakesNoMoreThanOneFramePerInterval) {
  // intervalMs is a floor rather than a cadence (ADR 0018), and a burst that ignored it would
  // take every frame on consecutive ticks — five frames 16ms apart, which is one motion-blurred
  // frame photographed five times rather than five chances at a sharp one.
  Begin();
  BurstSpec burst;
  burst.frameCount = 3;
  burst.intervalMs = 80;
  const NodeId node = FirstNode();
  ASSERT_TRUE(manager->ArmBurst(node, burst).ok());

  ASSERT_TRUE(manager->OnMotion({}).ok());
  ASSERT_EQ(manager->Candidates(node).value.size(), 1u);
  // Four more ticks inside the same interval. The capture loop runs at animation rate, so this
  // is what actually happens between one burst frame and the next.
  for (int i = 0; i < 4; ++i) {
    clock.AdvanceMs(10);
    ASSERT_TRUE(manager->OnMotion({}).ok());
  }
  EXPECT_EQ(manager->Candidates(node).value.size(), 1u) << "the interval was not honoured";

  clock.AdvanceMs(burst.intervalMs);
  ASSERT_TRUE(manager->OnMotion({}).ok());
  EXPECT_EQ(manager->Candidates(node).value.size(), 2u);
}

TEST_F(CaptureSession, GuidanceTargetsTheArmedCellWhileFiring) {
  // A burst that retargeted itself because the user drifted off the cell mid-flight would end up
  // with candidates from two cells in one set, which is a ghost nobody could explain later.
  //
  // This one needs the real tessellation. The null planner makes a one-cell plan, where "the cell
  // being fired at" and "the nearest cell" are the same answer and the assertion cannot fail —
  // which it did not, until a deliberate sabotage of the line under test failed to turn it red.
  RingsCoveragePlannerEngine rings;
  CaptureSessionManager real(rings, pose, quality, *camera, *sensor, *store, *projects, clock);
  CapturePlanSpec spec = Spec();
  spec.horizontalFovDeg = 66.0;
  spec.verticalFovDeg = 50.0;
  ASSERT_TRUE(real.Begin(kProject, spec).ok());
  const std::vector<CoverageNode> nodes = real.GetPlan().value.nodes;
  ASSERT_GT(nodes.size(), 8u);

  auto aimed = real.OnMotion({});
  ASSERT_TRUE(aimed.ok()) << aimed.status.detail;
  const NodeId nearest = aimed.value.targetNode;

  NodeId elsewhere = nearest;
  for (const auto& node : nodes) {
    if (node.id.value != nearest.value) { elsewhere = node.id; break; }
  }
  ASSERT_NE(elsewhere.value, nearest.value);

  BurstSpec burst;
  burst.frameCount = 3;
  ASSERT_TRUE(real.ArmBurst(elsewhere, burst).ok());
  auto firing = real.OnMotion({});
  ASSERT_TRUE(firing.ok()) << firing.status.detail;
  EXPECT_EQ(firing.value.targetNode.value, elsewhere.value);
  EXPECT_EQ(firing.value.action, GuidanceAction::Firing);
}

TEST_F(CaptureSession, ACameraThatStopsProducingFramesAbandonsTheBurstRatherThanHoldingTheLock) {
  // Preview is running by the time a burst is armed, so a peek that fails has failed for real.
  // Retrying it every tick forever would hold the exposure locked while doing it.
  Begin();
  BurstSpec burst;
  burst.frameCount = 4;
  burst.lockExposure = true;
  const NodeId node = FirstNode();
  ASSERT_TRUE(manager->ArmBurst(node, burst).ok());
  ASSERT_TRUE(manager->OnMotion({}).ok());
  ASSERT_EQ(manager->Candidates(node).value.size(), 1u);

  ASSERT_TRUE(camera->StopPreview().ok());
  clock.AdvanceMs(burst.intervalMs);
  EXPECT_EQ(manager->OnMotion({}).status.code, StatusCode::FailedPrecondition);

  EXPECT_FALSE(camera->ExposureLocked());
  EXPECT_TRUE(manager->Candidates(node).value.empty()) << "the half-burst was rolled back";
  // And the session is usable again rather than stuck firing.
  ASSERT_TRUE(camera->StartPreview().ok());
  EXPECT_TRUE(manager->ArmBurst(node, burst).ok());
}

TEST_F(CaptureSession, ARetakeOfTheCellBeingFiredAtAbandonsThatBurst) {
  // The burst was taken for the set the caller is about to change, and its rollback mark points
  // into a vector a replacing retake is about to empty.
  Begin();
  BurstSpec burst;
  burst.frameCount = 4;
  const NodeId node = FirstNode();
  ASSERT_TRUE(manager->ArmBurst(node, burst).ok());
  ASSERT_TRUE(manager->OnMotion({}).ok());
  const int64_t held = store->Budget().value.heapUsedBytes;
  ASSERT_GT(held, 0);

  ASSERT_TRUE(manager->RequestRetake(node, /*replace=*/false).ok());
  EXPECT_TRUE(manager->Candidates(node).value.empty());
  EXPECT_LT(store->Budget().value.heapUsedBytes, held);
  EXPECT_FALSE(camera->ExposureLocked());
}

TEST_F(CaptureSession, EndingMidBurstRollsItBackRatherThanBankingAHalfBurst) {
  // A burst that never completed was never ranked, so its frames are not evidence: keeping them
  // would let a half-burst count toward coverage on the next resume.
  Begin();
  BurstSpec burst;
  burst.frameCount = 5;
  const NodeId node = FirstNode();
  const int64_t before = store->Budget().value.heapUsedBytes;
  ASSERT_TRUE(manager->ArmBurst(node, burst).ok());
  ASSERT_TRUE(manager->OnMotion({}).ok());
  ASSERT_GT(store->Budget().value.heapUsedBytes, before);

  ASSERT_TRUE(manager->End().ok());
  EXPECT_EQ(store->Budget().value.heapUsedBytes, before);
}

TEST_F(CaptureSession, CapturingAnUnknownCellIsRefused) {
  Begin();
  EXPECT_EQ(manager->ArmBurst(NodeId{9999}, BurstSpec{}).code, StatusCode::NotFound);
}

TEST_F(CaptureSession, CandidatesAccumulateAcrossBursts) {
  // A retake adds evidence rather than replacing it, so the pool has to grow.
  Begin();
  BurstSpec burst;
  burst.frameCount = 2;
  const NodeId node = FirstNode();
  ASSERT_TRUE(FireBurst(node, burst).ok());
  ASSERT_TRUE(FireBurst(node, burst).ok());
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
  ASSERT_TRUE(FireBurst(FirstNode(), burst).ok());

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
  ASSERT_TRUE(FireBurst(node, burst).ok());
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
  ASSERT_TRUE(FireBurst(node, burst).ok());
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
  ASSERT_TRUE(FireBurst(node, burst).ok());
  const int64_t held = store->Budget().value.heapUsedBytes;
  ASSERT_GT(held, 0);

  ASSERT_TRUE(manager->RequestRetake(node, /*replace=*/true).ok());
  EXPECT_LT(store->Budget().value.heapUsedBytes, held);
}

TEST_F(CaptureSession, EndPersistsTheSessionSoItCanBeResumed) {
  Begin();
  BurstSpec burst;
  burst.frameCount = 1;
  ASSERT_TRUE(FireBurst(FirstNode(), burst).ok());
  ASSERT_TRUE(manager->End().ok());
  EXPECT_GT(projects->WriteCount(), 0);
}

TEST_F(CaptureSession, EndClosesTheCameraSoTheIndicatorGoesOut) {
  // A session that leaves the camera running reads to the user as the app watching them.
  Begin();
  ASSERT_TRUE(manager->End().ok());
  EXPECT_EQ(camera->PeekPreviewFrame().status.code, StatusCode::FailedPrecondition);
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
