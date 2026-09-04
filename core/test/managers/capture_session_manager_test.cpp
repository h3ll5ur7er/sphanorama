// The capture session's sequence, from docs/03 UC-1.
//
// What is under test is *when* the manager asks each collaborator, never what they answer — the
// answers belong to engines behind contracts. Driving it with fakes is only cheap because the
// camera and sensors are resource-access contracts rather than browser calls, which is the whole
// argument for that layer.
#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

#include "engines/coverage_planner_engine/null_coverage_planner_engine.h"
#include "engines/frame_quality_engine/null_frame_quality_engine.h"
#include "engines/coverage_planner_engine/rings_coverage_planner_engine.h"
#include "engines/pose_engine/null_pose_engine.h"
#include "managers/capture_session_manager/capture_session_manager.h"
#include "engines/frame_quality_engine/sharpness_frame_quality_engine.h"
#include "support/fake_camera_access.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "support/fake_motion_sensor_access.h"
#include "support/fake_spill_sink.h"
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

// Ranks backwards through capture order. What matters is only that it disagrees with the order
// frames arrived in, so a manager that hands back capture order cannot pass by accident.
class ReversedQualityEngine final : public IFrameQualityEngine {
 public:
  Result<QualityScore> Score(const FrameRef&, const PoseSample&, const NodeContext&) override {
    return Ok(QualityScore{});
  }

  Result<std::vector<CandidateId>> Rank(std::span<const Candidate> candidates,
                                        const SelectionPolicy&) override {
    if (fail_) {
      return Err<std::vector<CandidateId>>(StatusCode::Internal, "test", "no ranking today");
    }
    std::vector<CandidateId> ranked;
    ranked.reserve(candidates.size());
    for (auto it = candidates.rbegin(); it != candidates.rend(); ++it) ranked.push_back(it->id);
    // A misbehaving engine, on demand: naming the same candidate twice is as easy a mistake to
    // make as omitting one, and the manager already promises to survive the omission.
    if (repeat_ && !ranked.empty()) ranked.push_back(ranked.front());
    return Ok(std::move(ranked));
  }

  void FailRanking(bool fail) { fail_ = fail; }
  void RepeatTheBest(bool repeat) { repeat_ = repeat; }

 private:
  bool fail_ = false;
  bool repeat_ = false;
};

class CaptureSession : public ::testing::Test {
 protected:
  void SetUp() override {
    store = std::make_shared<MemoryFrameStoreAccess>(1 << 22);
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
  std::shared_ptr<MemoryFrameStoreAccess> store;
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

// Hands back a starting state and then refuses to integrate, so a tick can fail on the pose
// rather than on the planner. Both are early returns from OnMotion and they are separate lines;
// testing one and assuming the other is how the second stays broken.
class UnintegrablePoseEngine final : public IPoseEngine {
 public:
  Result<PoseState> Initial(PoseMode mode, MotionCapability capability) override {
    return inner_.Initial(mode, capability);
  }
  Result<PoseState> Integrate(const PoseState&, std::span<const ImuSample>) override {
    return Err<PoseState>(StatusCode::ComputeUnavailable, "test", "cannot integrate");
  }
  Result<PoseSample> Correct(const FrameRef& current, const FrameRef& reference,
                             const PoseSample& prior) override {
    return inner_.Correct(current, reference, prior);
  }
  Result<double> Stability(std::span<const ImuSample> samples) override {
    return inner_.Stability(samples);
  }

 private:
  NullPoseEngine inner_;
};

// Plans fine and then refuses to locate, which is what makes an OnMotion that fails *before* the
// burst is advanced reachable. The refusing planner above cannot: it fails Plan too, so no session
// ever begins and no burst is ever armed.
class UnlocatablePlannerEngine final : public ICoveragePlannerEngine {
 public:
  Result<CapturePlan> Plan(const CapturePlanSpec& spec, const Intrinsics& lens) override {
    return inner_.Plan(spec, lens);
  }
  Result<CaptureGuidance> Locate(const Quat&, const CapturePlan&, const CoverageState&) override {
    return Err<CaptureGuidance>(StatusCode::Unsupported, "test", "cannot locate");
  }
  Result<CoverageState> Evaluate(const CapturePlan& plan,
                                 std::span<const Candidate> candidates) override {
    return inner_.Evaluate(plan, candidates);
  }
  Result<std::vector<NodeId>> SuggestRetakes(const CapturePlan& plan, const CoverageState& state,
                                             const GhostReport& ghosts) override {
    return inner_.SuggestRetakes(plan, state, ghosts);
  }

 private:
  NullCoveragePlannerEngine inner_;
};

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
  Result<CaptureGuidance> Locate(const Quat&, const CapturePlan&, const CoverageState&) override {
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

TEST_F(CaptureSession, GuidanceStopsAskingForACellOnceItIsCaptured) {
  // The engine decides what "still needed" means, but only if the manager actually hands it the
  // coverage — and a manager that passed a default-constructed state would look identical from
  // the engine's own tests. The null planner lays a single cell, so filling it leaves nothing
  // missing, and a sphere with nothing missing says so.
  Begin();
  const NodeId node = FirstNode();
  // Aimed at it already — the null plan's one cell sits at identity and so does the starting
  // pose — so the answer before capturing is to hold still, not to go looking.
  EXPECT_EQ(manager->OnMotion({}).value.action, GuidanceAction::HoldStill);

  auto frame = store->Allocate(4, 4, PixelFormat::RGBA8);
  ASSERT_TRUE(frame.ok());
  ASSERT_TRUE(manager->OfferFrame(node, frame.value, PoseSample{}).ok());

  auto guidance = manager->OnMotion({});
  ASSERT_TRUE(guidance.ok());
  EXPECT_EQ(guidance.value.action, GuidanceAction::SphereDone);
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
  EXPECT_EQ(camera->FramesTaken(), 0);

  // Ticked by hand rather than through FireBurst, so the guidance the client would render on the
  // way is asserted too: Firing until the burst is full, CellDone exactly on the tick that
  // fills it, and never CellDone twice.
  for (int32_t taken = 1; taken <= burst.frameCount; ++taken) {
    auto guidance = manager->OnMotion({});
    ASSERT_TRUE(guidance.ok()) << guidance.status.detail;
    EXPECT_EQ(guidance.value.action,
              taken == burst.frameCount ? GuidanceAction::CellDone : GuidanceAction::Firing)
        << "on frame " << taken;
    // The frame was taken from the camera, and the cell has not seen it. A burst becomes evidence
    // when the whole of it ranks, not as it goes: Coverage counts a cell satisfied as soon as one
    // candidate exists for it, so a cell filling in public would report itself complete on the
    // first frame of a burst that could still roll back.
    EXPECT_EQ(camera->FramesTaken(), taken);
    if (taken < burst.frameCount) {
      EXPECT_TRUE(manager->Candidates(node).value.empty()) << "committed early, on frame " << taken;
    }
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

  // Counted at the camera rather than in the cell: an in-flight burst is deliberately invisible
  // through Candidates until it ranks, and what this test is about is how often the port is read.
  ASSERT_TRUE(manager->OnMotion({}).ok());
  ASSERT_EQ(camera->FramesTaken(), 1);
  // Four more ticks inside the same interval. The capture loop runs at animation rate, so this
  // is what actually happens between one burst frame and the next.
  for (int i = 0; i < 4; ++i) {
    clock.AdvanceMs(10);
    ASSERT_TRUE(manager->OnMotion({}).ok());
  }
  EXPECT_EQ(camera->FramesTaken(), 1) << "the interval was not honoured";

  clock.AdvanceMs(burst.intervalMs);
  ASSERT_TRUE(manager->OnMotion({}).ok());
  EXPECT_EQ(camera->FramesTaken(), 2);
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
  ASSERT_EQ(camera->FramesTaken(), 1);

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

TEST_F(CaptureSession, RefusesANegativeInterval) {
  // The arithmetic that paces a burst makes a negative interval always overdue, so instead of
  // failing it would quietly capture on every tick at whatever rate the client runs at. Refused
  // before the locks are taken, so a rejected spec leaves the camera as it found it.
  Begin();
  BurstSpec burst;
  burst.intervalMs = -1;
  EXPECT_EQ(manager->ArmBurst(FirstNode(), burst).code, StatusCode::InvalidArgument);
  EXPECT_FALSE(camera->ExposureLocked());
}

TEST_F(CaptureSession, ACellWhoseBurstIsStillInFlightIsNotCountedAsCovered) {
  // Evaluate marks a node covered as soon as any candidate exists for it, so a burst that filled
  // the cell as it went would report the cell complete on its first frame — while still able to
  // roll back. A sphere could then read as finished and contain a cell nothing ever ranked.
  Begin();
  BurstSpec burst;
  burst.frameCount = 3;
  const NodeId node = FirstNode();
  ASSERT_TRUE(manager->ArmBurst(node, burst).ok());
  ASSERT_TRUE(manager->OnMotion({}).ok());
  ASSERT_EQ(camera->FramesTaken(), 1) << "the burst did not take a frame";

  auto midway = manager->Coverage();
  ASSERT_TRUE(midway.ok());
  EXPECT_EQ(midway.value.nodesSatisfied, 0);

  // Ticked to completion by hand: the burst already in flight cannot be armed a second time.
  for (int32_t remaining = 1; remaining < burst.frameCount; ++remaining) {
    clock.AdvanceMs(burst.intervalMs);
    ASSERT_TRUE(manager->OnMotion({}).ok());
  }
  ASSERT_EQ(manager->Candidates(node).value.size(), 3u) << "the burst never committed";

  auto after = manager->Coverage();
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after.value.nodesSatisfied, 1) << "and it is counted once the burst ranks";
}

TEST_F(CaptureSession, AFrameOfferedDuringABurstSurvivesThatBurstRollingBack) {
  // OfferFrame's frames belong to the caller, and forgetting someone else's handle is the worse
  // bug — its own comment says so. An index into the cell was not an ownership boundary, because
  // an offer lands in the same vector; the burst's frames are held apart from it instead.
  RefusingFrameQualityEngine refusing;
  NullFrameQualityEngine scoring;
  Begin();
  const NodeId node = FirstNode();

  auto offered = store->Allocate(4, 4, PixelFormat::RGBA8);
  ASSERT_TRUE(offered.ok());
  ASSERT_TRUE(manager->OfferFrame(node, offered.value, PoseSample{}).ok());
  ASSERT_EQ(manager->Candidates(node).value.size(), 1u);

  // A burst over the same cell that then fails to score, taking its own frames back with it.
  BurstSpec burst;
  burst.frameCount = 2;
  ASSERT_TRUE(manager->ArmBurst(node, burst).ok());
  ASSERT_TRUE(camera->StopPreview().ok());   // the next peek fails, abandoning the burst
  EXPECT_FALSE(manager->OnMotion({}).ok());

  auto survivors = manager->Candidates(node);
  ASSERT_TRUE(survivors.ok());
  ASSERT_EQ(survivors.value.size(), 1u) << "the offered frame went with the burst";
  EXPECT_EQ(survivors.value.front().frame.id.value, offered.value.id.value);
  // And it is still in the store, not forgotten on someone else's behalf.
  EXPECT_TRUE(store->ResidencyOf(offered.value).ok());
}

TEST_F(CaptureSession, ATickThatFailsBeforeTheBurstStillGivesTheLocksBack) {
  // The burst is advanced at the end of OnMotion, so a pose or planner failure returns before it.
  // Left armed, the exposure stays locked — and the client stops ticking once a call fails, so
  // nothing would ever reach the cleanup. The lock would outlive the session.
  UnlocatablePlannerEngine unlocatable;
  CaptureSessionManager manager(unlocatable, pose, quality, *camera, *sensor, *store, *projects,
                                clock);
  ASSERT_TRUE(manager.Begin(kProject, Spec()).ok());
  const NodeId node = manager.GetPlan().value.nodes.front().id;

  BurstSpec burst;
  burst.frameCount = 3;
  burst.lockExposure = true;
  ASSERT_TRUE(manager.ArmBurst(node, burst).ok());
  ASSERT_TRUE(camera->ExposureLocked());

  EXPECT_EQ(manager.OnMotion({}).status.code, StatusCode::Unsupported);
  EXPECT_FALSE(camera->ExposureLocked()) << "the failing tick left the camera locked";
  // And the burst is gone rather than half-armed, so the cell can be tried again.
  EXPECT_TRUE(manager.ArmBurst(node, burst).ok());
}

TEST_F(CaptureSession, ATickWhosePoseFailsAlsoGivesTheLocksBack) {
  // The planner is not the only way out of OnMotion before the burst is advanced; the pose is the
  // other, and they are separate lines. A sabotage of the pose guard passed every test until this
  // one existed, which is the whole argument for writing it.
  UnintegrablePoseEngine unintegrable;
  CaptureSessionManager manager(planner, unintegrable, quality, *camera, *sensor, *store,
                                *projects, clock);
  ASSERT_TRUE(manager.Begin(kProject, Spec()).ok());
  const NodeId node = manager.GetPlan().value.nodes.front().id;

  BurstSpec burst;
  burst.frameCount = 3;
  burst.lockExposure = true;
  ASSERT_TRUE(manager.ArmBurst(node, burst).ok());
  ASSERT_TRUE(camera->ExposureLocked());

  // A non-empty batch, or Integrate is never reached and the tick cannot fail there.
  std::vector<ImuSample> samples(2);
  EXPECT_EQ(manager.OnMotion(samples).status.code, StatusCode::ComputeUnavailable);
  EXPECT_FALSE(camera->ExposureLocked()) << "the failing tick left the camera locked";
  EXPECT_TRUE(manager.ArmBurst(node, burst).ok());
}

TEST_F(CaptureSession, AnUnlockThatFailsIsReportedRatherThanSwallowed) {
  // The port is fallible — applyConstraints can reject — and a discarded result would report a
  // captured cell while the camera stayed locked, with nothing left holding that knowledge.
  Begin();
  BurstSpec burst;
  burst.frameCount = 2;
  const NodeId node = FirstNode();
  ASSERT_TRUE(manager->ArmBurst(node, burst).ok());
  camera->FailUnlock(true);

  ASSERT_TRUE(manager->OnMotion({}).ok());
  clock.AdvanceMs(burst.intervalMs);
  auto completing = manager->OnMotion({});
  EXPECT_EQ(completing.status.code, StatusCode::CameraUnavailable);

  // The cell is captured all the same: the frames ranked, and throwing them away because the
  // camera would not unlock would lose real evidence over a separate problem.
  EXPECT_EQ(manager->Candidates(node).value.size(), 2u);
  // And the burst is finished rather than stuck, so the next arm can try the locks again.
  camera->FailUnlock(false);
  EXPECT_TRUE(manager->ArmBurst(node, burst).ok());
}

TEST_F(CaptureSession, ATickThatFailsWhileTheUnlockAlsoFailsReportsBothRatherThanOne) {
  // Two failures at once, one Status to say them in. The cause keeps the code because it is what
  // the caller has to act on; the unlock rides along in the detail. Returning the cause alone is
  // what this used to do — and the client stops ticking once a call fails, so the lock would
  // outlive the session with nothing anywhere holding the knowledge that it had been taken.
  UnintegrablePoseEngine unintegrable;
  CaptureSessionManager manager(planner, unintegrable, quality, *camera, *sensor, *store,
                                *projects, clock);
  ASSERT_TRUE(manager.Begin(kProject, Spec()).ok());
  const NodeId node = manager.GetPlan().value.nodes.front().id;

  BurstSpec burst;
  burst.frameCount = 3;
  burst.lockExposure = true;
  ASSERT_TRUE(manager.ArmBurst(node, burst).ok());
  camera->FailUnlock(true);

  // A non-empty batch, or Integrate is never reached and the tick cannot fail there.
  std::vector<ImuSample> samples(2);
  const Status failed = manager.OnMotion(samples).status;
  EXPECT_EQ(failed.code, StatusCode::ComputeUnavailable) << "the cause was replaced, not kept";
  EXPECT_NE(failed.detail.find("still locked"), std::string::npos)
      << "the unlock failure was discarded: " << failed.detail;
}

TEST_F(CaptureSession, ARetakeThatCannotGiveTheLocksBackSaysSoAndChangesNothing) {
  // A retake that answered Ok while the camera stayed pinned to the abandoned burst's exposure
  // would send the user off to re-aim at a viewfinder that cannot respond. Reported before the
  // candidate set is touched, so a caller that sees this and asks again gets the retake it wanted.
  Begin();
  BurstSpec burst;
  burst.frameCount = 2;
  burst.lockExposure = true;
  const NodeId node = FirstNode();
  ASSERT_TRUE(FireBurst(node, burst).ok());
  ASSERT_EQ(manager->Candidates(node).value.size(), 2u);

  ASSERT_TRUE(manager->ArmBurst(node, burst).ok());
  camera->FailUnlock(true);
  EXPECT_EQ(manager->RequestRetake(node, true).code, StatusCode::CameraUnavailable);
  EXPECT_EQ(manager->Candidates(node).value.size(), 2u)
      << "the evidence was discarded by a retake that reported failure";

  // And the second attempt does what was asked, because the burst itself is already gone.
  camera->FailUnlock(false);
  EXPECT_TRUE(manager->RequestRetake(node, true).ok());
  EXPECT_TRUE(manager->Candidates(node).value.empty());
}

TEST_F(CaptureSession, EndingMidBurstIsCleanWhenTheCameraClosesDespiteARefusedUnlock) {
  // Close stops the track, and a stopped track has no locks: the unlock failure is moot rather
  // than hidden. Reporting it here would tell the caller a session that ended cleanly did not,
  // and there is nothing they could do about it either way.
  Begin();
  BurstSpec burst;
  burst.frameCount = 3;
  burst.lockExposure = true;
  ASSERT_TRUE(manager->ArmBurst(FirstNode(), burst).ok());
  camera->FailUnlock(true);

  EXPECT_TRUE(manager->End().ok());
  EXPECT_FALSE(camera->IsOpen());
}

TEST_F(CaptureSession, EndingMidBurstReportsTheRefusedUnlockWhenTheCameraStaysOpenToo) {
  // The other half of the same rule. A close that failed leaves a camera that is both open and
  // still locked, and that is worth saying even though the session itself ended.
  Begin();
  BurstSpec burst;
  burst.frameCount = 3;
  burst.lockExposure = true;
  ASSERT_TRUE(manager->ArmBurst(FirstNode(), burst).ok());
  camera->FailUnlock(true);
  camera->FailClose(true);

  const Status ended = manager->End();
  EXPECT_EQ(ended.code, StatusCode::CameraUnavailable);
  EXPECT_NE(ended.detail.find("still locked"), std::string::npos)
      << "only one of the two failures was reported: " << ended.detail;

  // The session ended regardless: every field was cleared before that status was returned, so a
  // failure here is a report and not a half-ended session nothing can get out of.
  camera->FailClose(false);
  EXPECT_TRUE(manager->Begin(kProject, Spec()).ok());
}

TEST_F(CaptureSession, ABurstTakesNoFramesFasterThanTheCameraCanMakeThem) {
  // PeekPreviewFrame borrows the *latest* frame, so ticking faster than the camera produces them
  // does not capture faster — it captures the same frame twice. The fake reports 30 fps, so a
  // zero-interval burst in a 60 Hz loop would fill with duplicates and selection would rank one
  // exposure against copies of itself: worse than a slow burst, because it looks like a fast one.
  Begin();
  BurstSpec burst;
  burst.frameCount = 3;
  burst.intervalMs = 0;  // "as fast as you can", which is the camera's business to answer
  ASSERT_TRUE(manager->ArmBurst(FirstNode(), burst).ok());

  ASSERT_TRUE(manager->OnMotion({}).ok());
  ASSERT_EQ(camera->FramesTaken(), 1);
  // Four ticks inside the camera's 33.3 ms period, at about the rate the capture loop runs.
  for (int i = 0; i < 4; ++i) {
    clock.AdvanceMs(8);
    ASSERT_TRUE(manager->OnMotion({}).ok());
  }
  EXPECT_EQ(camera->FramesTaken(), 1) << "the camera's own rate was ignored";

  clock.AdvanceMs(8);
  ASSERT_TRUE(manager->OnMotion({}).ok());
  EXPECT_EQ(camera->FramesTaken(), 2);
}

TEST_F(CaptureSession, ACameraThatWillNotSayItsRateLeavesTheSpecInCharge) {
  // maxBurstFps is 0 when the platform will not report one, and 0 has to mean "no floor here"
  // rather than a guess. A default invented in the manager would slow every burst on the browsers
  // that decline to answer, which is most of them.
  CameraCapabilities silent = camera->Capabilities();
  silent.maxBurstFps = 0;
  camera->SetCapabilities(silent);
  Begin();

  BurstSpec burst;
  burst.frameCount = 3;
  burst.intervalMs = 0;
  ASSERT_TRUE(manager->ArmBurst(FirstNode(), burst).ok());

  // Two ticks with the clock standing still. The tick rate is the only floor left.
  ASSERT_TRUE(manager->OnMotion({}).ok());
  ASSERT_TRUE(manager->OnMotion({}).ok());
  EXPECT_EQ(camera->FramesTaken(), 2);
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

TEST_F(CaptureSession, ACellIsStillCapturedWhenTheStoreHasNoSpillTier) {
  // This fixture's store has no sink, which is the native build and any browser whose OPFS handle
  // did not open. Spilling a committed cell is an optimisation, so a store that cannot do it must
  // still capture: a refusal from the tier is not a refusal of the capture.
  Begin();
  BurstSpec burst;
  burst.frameCount = 3;
  const NodeId node = FirstNode();
  ASSERT_TRUE(FireBurst(node, burst).ok());

  const std::vector<Candidate> captured = manager->Candidates(node).value;
  ASSERT_EQ(captured.size(), 3u);
  for (const auto& candidate : captured) {
    EXPECT_NE(store->ResidencyOf(candidate.frame).value, Residency::Spilled);
  }
}

TEST_F(CaptureSession, ASessionCanBeBegunAgainAfterEnding) {
  const SessionId first = Begin();
  ASSERT_TRUE(manager->End().ok());
  const SessionId second = Begin();
  EXPECT_NE(first.value, second.value);
  EXPECT_TRUE(manager->Candidates(FirstNode()).value.empty());
}

// ------------------------------------------------------------------- under memory pressure
//
// A sphere of bursts does not fit in a phone's frame store — that is the premise the whole spill
// tier exists for, and the fixture above cannot reach it: its ceiling holds a thousand of the
// fake camera's tiny frames and its store has nowhere to spill to. This one has a sink and a
// ceiling smaller than the capture it is asked to complete.

// The fake camera's preview frame, 32x24 RGBA. Stated rather than derived, because the ceiling
// has to be chosen before the camera is opened — and asserted below against what actually reaches
// the sink, so a change to the fake fails a test instead of quietly making these ceilings roomy.
constexpr int64_t kPreviewFrameBytes = 32 * 24 * 4;

class CaptureSessionUnderPressure : public ::testing::Test {
 protected:
  // Eight frames: enough for a burst of three, the siblings a retake faults back in to score
  // against, and nothing like enough for the sphere the test below captures.
  static constexpr int64_t kCeilingBytes = kPreviewFrameBytes * 8;

  void SetUp() override {
    store = std::make_shared<MemoryFrameStoreAccess>(kCeilingBytes, &sink);
    camera = std::make_unique<FakeCameraAccess>(store);
    sensor = std::make_unique<FakeMotionSensorAccess>();
    projects = std::make_unique<FakeProjectStoreAccess>();
    (void)projects->WriteDocument(kProject, "title", "test project");
    manager = std::make_unique<CaptureSessionManager>(rings, pose, quality, *camera, *sensor,
                                                      *store, *projects, clock);
  }

  Status FireBurst(NodeId node, const BurstSpec& burst) {
    return FireBurstOn(*manager, clock, node, burst);
  }

  // A real tessellation, because one cell cannot overrun a ceiling that holds a burst.
  CapturePlanSpec Spec() {
    CapturePlanSpec spec;
    spec.acceptanceConeDeg = 5.0;
    spec.horizontalFovDeg = 66.0;
    spec.verticalFovDeg = 50.0;
    return spec;
  }

  void Begin() { ASSERT_TRUE(manager->Begin(kProject, Spec()).ok()); }

  // Declared before the store so it outlives the store that holds a pointer to it.
  FakeSpillSink sink;
  RingsCoveragePlannerEngine rings;
  NullPoseEngine pose;
  NullFrameQualityEngine quality;
  std::shared_ptr<MemoryFrameStoreAccess> store;
  std::unique_ptr<FakeCameraAccess> camera;
  std::unique_ptr<FakeMotionSensorAccess> sensor;
  std::unique_ptr<FakeProjectStoreAccess> projects;
  ManualClock clock;
  std::unique_ptr<CaptureSessionManager> manager;
};

TEST_F(CaptureSessionUnderPressure, ACapturedCellsFramesLeaveTheHeap) {
  // The moment a burst is ranked, its frames are cold: nothing looks at a captured cell's pixels
  // again until the build or the review client asks, and both of those go through Pin, which
  // faults them back in. Holding them in the heap until then is holding the whole sphere.
  Begin();
  BurstSpec burst;
  burst.frameCount = 3;
  const NodeId node = manager->GetPlan().value.nodes.front().id;
  ASSERT_TRUE(FireBurst(node, burst).ok());

  const std::vector<Candidate> captured = manager->Candidates(node).value;
  ASSERT_EQ(captured.size(), 3u);
  for (const auto& candidate : captured) {
    EXPECT_EQ(store->ResidencyOf(candidate.frame).value, Residency::Spilled);
  }
  EXPECT_EQ(store->Budget().value.heapUsedBytes, 0);
  // The bytes, not a relabelling — and the size the ceilings above were chosen from.
  ASSERT_TRUE(sink.Holds(captured.front().frame.id.value));
  EXPECT_EQ(sink.Held(captured.front().frame.id.value).size(),
            static_cast<size_t>(kPreviewFrameBytes));
}

TEST_F(CaptureSessionUnderPressure, ASphereLargerThanTheCeilingIsStillCaptured) {
  // The one that matters. Six cells of three frames is more than twice what this store can hold
  // at once, which is the phone's situation in miniature: without a policy the fourth cell's
  // first allocation is refused and the capture stops halfway round with no way forward.
  Begin();
  BurstSpec burst;
  burst.frameCount = 3;
  const std::vector<CoverageNode> nodes = manager->GetPlan().value.nodes;
  ASSERT_GT(nodes.size(), 6u);

  int64_t captured = 0;
  for (size_t cell = 0; cell < 6; ++cell) {
    const Status fired = FireBurst(nodes[cell].id, burst);
    ASSERT_TRUE(fired.ok()) << "cell " << cell << ": " << fired.detail;
    captured += kPreviewFrameBytes * burst.frameCount;
    EXPECT_LE(store->Budget().value.heapUsedBytes, kCeilingBytes);
  }
  // Says out loud that the capture did not fit, so this cannot pass by the ceiling being roomy.
  EXPECT_GT(captured, kCeilingBytes);
  EXPECT_EQ(store->Budget().value.spilledBytes, captured);
}

TEST_F(CaptureSessionUnderPressure, ASinkThatRefusesTheWriteDoesNotCostTheCell) {
  // A phone with no quota left. The cell is already ranked and committed by the time anything is
  // sent to the sink, so a refusal there is a capture that could not be made cheaper — not a
  // capture that failed. The frames stay in the heap and the session carries on until an
  // allocation genuinely does not fit, which is the honest place to find out.
  sink.FailWrites(true);
  Begin();
  BurstSpec burst;
  burst.frameCount = 3;
  const NodeId node = manager->GetPlan().value.nodes.front().id;
  ASSERT_TRUE(FireBurst(node, burst).ok());

  EXPECT_EQ(manager->Candidates(node).value.size(), 3u);
  EXPECT_EQ(store->Budget().value.heapUsedBytes, kPreviewFrameBytes * 3);
}

TEST_F(CaptureSessionUnderPressure, AnOfferedFrameIsNotCooledByALaterBurst) {
  // The cell is not an ownership boundary. `OfferFrame` appends the caller's own candidate to the
  // same vector a burst commits into, so cooling the vector wholesale demotes a handle the
  // manager never owned — and the caller's next Pin then faults from a sink it does not know
  // exists, or is refused at a ceiling it has no way to see.
  //
  // The manager's own comment on `pending_` says exactly this about a rollback mark. The same
  // vector, the same reason, one line further down.
  Begin();
  const NodeId node = manager->GetPlan().value.nodes.front().id;

  auto borrowed = store->Allocate(8, 8, PixelFormat::RGBA8);
  ASSERT_TRUE(borrowed.ok());
  ASSERT_TRUE(manager->OfferFrame(node, borrowed.value, PoseSample{}).ok());

  BurstSpec burst;
  burst.frameCount = 3;
  ASSERT_TRUE(FireBurst(node, burst).ok());

  EXPECT_NE(store->ResidencyOf(borrowed.value).value, Residency::Spilled)
      << "a frame the caller still owns was sent to the sink";
  // And the burst's own frames still went, so this is not the policy being switched off.
  const std::vector<Candidate> captured = manager->Candidates(node).value;
  ASSERT_EQ(captured.size(), 4u);
  int spilled = 0;
  for (const auto& candidate : captured) {
    if (store->ResidencyOf(candidate.frame).value == Residency::Spilled) ++spilled;
  }
  EXPECT_EQ(spilled, 3);
}

TEST_F(CaptureSessionUnderPressure, ARetakeThatIsAbandonedStillCoolsWhatItFaultedIn) {
  // Cooling on the way out of a *successful* burst only is a leak with a plausible cover story.
  // Scoring a retake reads its siblings, which faults the cell's spilled frames back into the
  // heap and leaves them there; if that burst then ends any other way — a failure, or the retake
  // this test uses — nothing sends them back down, and they hold the ceiling for the rest of the
  // session. Which is the allocation failure the whole policy exists to prevent, arriving by the
  // one path that skips the policy.
  SharpnessFrameQualityEngine sharp{*store};
  CaptureSessionManager real(rings, pose, sharp, *camera, *sensor, *store, *projects, clock);
  ASSERT_TRUE(real.Begin(kProject, Spec()).ok());
  BurstSpec burst;
  burst.frameCount = 3;
  const NodeId node = real.GetPlan().value.nodes.front().id;
  ASSERT_TRUE(FireBurstOn(real, clock, node, burst).ok());
  ASSERT_EQ(store->Budget().value.heapUsedBytes, 0);

  // A second burst on the same cell, taken far enough to score against the spilled siblings and
  // then abandoned rather than completed.
  ASSERT_TRUE(real.RequestRetake(node, /*replace=*/false).ok());
  ASSERT_TRUE(real.ArmBurst(node, burst).ok());
  ASSERT_TRUE(real.OnMotion({}).ok());
  clock.AdvanceMs(burst.intervalMs);
  ASSERT_TRUE(real.OnMotion({}).ok());
  ASSERT_GT(store->Budget().value.heapUsedBytes, 0) << "nothing was faulted in to leave behind";

  ASSERT_TRUE(real.RequestRetake(node, /*replace=*/false).ok());

  for (const auto& candidate : real.Candidates(node).value) {
    EXPECT_EQ(store->ResidencyOf(candidate.frame).value, Residency::Spilled);
  }
  EXPECT_EQ(store->Budget().value.heapUsedBytes, 0);
}

TEST_F(CaptureSessionUnderPressure, ARetakeIsScoredAgainstEvidenceThatLeftTheHeap) {
  // A retake adds to the evidence pool, and scoring a frame against its siblings reads their
  // pixels — which are in the sink by then. This is the interaction that would make spilling
  // quietly lossy: siblings that cannot be read are skipped rather than reported, so a broken
  // fault-in would show up as exposure agreement silently computed against nothing.
  SharpnessFrameQualityEngine sharp{*store};
  CaptureSessionManager real(rings, pose, sharp, *camera, *sensor, *store, *projects, clock);
  ASSERT_TRUE(real.Begin(kProject, Spec()).ok());
  BurstSpec burst;
  burst.frameCount = 3;
  const NodeId node = real.GetPlan().value.nodes.front().id;
  ASSERT_TRUE(FireBurstOn(real, clock, node, burst).ok());

  ASSERT_TRUE(real.RequestRetake(node, /*replace=*/false).ok());
  const Status again = FireBurstOn(real, clock, node, burst);
  ASSERT_TRUE(again.ok()) << again.detail;
  const std::vector<Candidate> pool = real.Candidates(node).value;
  ASSERT_EQ(pool.size(), 6u);

  // And they go back down, all six. Scoring faulted the first burst's frames into the heap to
  // read them, so a policy that cooled only the frames its own burst took would leave them there
  // — and a cell retaken a few times would sit in the heap for the rest of the session, which is
  // the failure this exists to prevent, arriving by the door marked "already handled".
  for (const auto& candidate : pool) {
    EXPECT_EQ(store->ResidencyOf(candidate.frame).value, Residency::Spilled);
  }
  EXPECT_EQ(store->Budget().value.heapUsedBytes, 0);
}

// ------------------------------------------------------------------ the order they come back in
//
// `Rank` is where "best" is decided (V6), and the manager already asks it on every committed
// burst — and threw the answer away, so `Candidates` handed back capture order. A review client
// showing that strip would either display frames in the order the shutter fired, which is not an
// opinion about anything, or rank them itself, which is the engine's job in the client's hands.

TEST_F(CaptureSession, CandidatesComeBackRankedRatherThanInCaptureOrder) {
  ReversedQualityEngine reversed;
  CaptureSessionManager ranked(planner, pose, reversed, *camera, *sensor, *store, *projects,
                               clock);
  ASSERT_TRUE(ranked.Begin(kProject, Spec()).ok());
  BurstSpec burst;
  burst.frameCount = 3;
  const NodeId node = ranked.GetPlan().value.nodes.front().id;
  ASSERT_TRUE(FireBurstOn(ranked, clock, node, burst).ok());

  const std::vector<Candidate> strip = ranked.Candidates(node).value;
  ASSERT_EQ(strip.size(), 3u);
  EXPECT_GT(strip[0].id.value, strip[1].id.value);
  EXPECT_GT(strip[1].id.value, strip[2].id.value);
}

TEST_F(CaptureSession, ARankingThatNamesACandidateTwiceDoesNotDuplicateIt) {
  // The other way a ranking can be wrong. Keeping candidates the ranking forgot is already
  // handled; a ranking that names one twice would grow the cell instead, and a cell holding two
  // entries with the same id has two frames as far as everything downstream can tell — the strip
  // shows both in force, and cooling and forgetting each run twice over one frame.
  ReversedQualityEngine reversed;
  reversed.RepeatTheBest(true);
  CaptureSessionManager ranked(planner, pose, reversed, *camera, *sensor, *store, *projects,
                               clock);
  ASSERT_TRUE(ranked.Begin(kProject, Spec()).ok());
  BurstSpec burst;
  burst.frameCount = 3;
  const NodeId node = ranked.GetPlan().value.nodes.front().id;
  ASSERT_TRUE(FireBurstOn(ranked, clock, node, burst).ok());

  const std::vector<Candidate> strip = ranked.Candidates(node).value;
  EXPECT_EQ(strip.size(), 3u);
  std::set<uint64_t> seen;
  for (const auto& candidate : strip) seen.insert(candidate.id.value);
  EXPECT_EQ(seen.size(), strip.size());
}

TEST_F(CaptureSession, AnOfferedFrameTakesItsPlaceInTheRankingRatherThanTheEnd) {
  // An imported frame is evidence like any other and may be the best of the set. Appending it
  // unranked would put a better frame behind worse ones for the rest of the session.
  ReversedQualityEngine reversed;
  CaptureSessionManager ranked(planner, pose, reversed, *camera, *sensor, *store, *projects,
                               clock);
  ASSERT_TRUE(ranked.Begin(kProject, Spec()).ok());
  BurstSpec burst;
  burst.frameCount = 2;
  const NodeId node = ranked.GetPlan().value.nodes.front().id;
  ASSERT_TRUE(FireBurstOn(ranked, clock, node, burst).ok());

  auto imported = store->Allocate(8, 8, PixelFormat::RGBA8);
  ASSERT_TRUE(imported.ok());
  ASSERT_TRUE(ranked.OfferFrame(node, imported.value, PoseSample{}).ok());

  const std::vector<Candidate> strip = ranked.Candidates(node).value;
  ASSERT_EQ(strip.size(), 3u);
  // Last to arrive, so first under this ranking — which is the point: position is the ranking's
  // to decide, not arrival's.
  EXPECT_EQ(strip[0].frame.id.value, imported.value.id.value);
}

TEST_F(CaptureSession, AnOfferThatCannotBeRankedLeavesTheCellAlone) {
  // Symmetric with the burst path, which rolls back a set nobody could rank. A cell holding a
  // candidate the selection engine has already failed on is worse than a rejected offer: the
  // failure is invisible and the strip is in an order nothing chose.
  ReversedQualityEngine reversed;
  CaptureSessionManager ranked(planner, pose, reversed, *camera, *sensor, *store, *projects,
                               clock);
  ASSERT_TRUE(ranked.Begin(kProject, Spec()).ok());
  BurstSpec burst;
  burst.frameCount = 2;
  const NodeId node = ranked.GetPlan().value.nodes.front().id;
  ASSERT_TRUE(FireBurstOn(ranked, clock, node, burst).ok());

  auto imported = store->Allocate(8, 8, PixelFormat::RGBA8);
  ASSERT_TRUE(imported.ok());
  reversed.FailRanking(true);
  EXPECT_FALSE(ranked.OfferFrame(node, imported.value, PoseSample{}).ok());
  EXPECT_EQ(ranked.Candidates(node).value.size(), 2u);
}

// A session that outlives the tab it was captured in. Everything below builds a second manager
// over a second store, because that is what a reload leaves standing: the project store's
// documents, and whatever the spill sink is holding. Nothing in the first manager's memory
// survives, so a test that reused it would be testing nothing.
class ResumedSession : public CaptureSession {
 protected:
  // A store with somewhere to spill, which the base fixture deliberately does not have: `Cool`
  // demotes a committed cell's frames (ADR 0023), and a store with no sink refuses that and keeps
  // them in a heap that is about to be destroyed.
  std::shared_ptr<MemoryFrameStoreAccess> NewStore() {
    return std::make_shared<MemoryFrameStoreAccess>(1 << 22, &sink);
  }

  FakeSpillSink sink;
};

TEST_F(ResumedSession, BringsBackTheCandidatesAndTheBytesBehindThem) {
  // The reload case, and the last code-shaped item in Phase 1's exit criterion. A call comes in
  // mid-capture and the tab is evicted; coming back has to find the cells already captured *and*
  // the pixels behind them. Coverage without pixels would be worse than starting over — a map
  // that says done, pointing at frames nothing can ever build from.
  auto first_store = NewStore();
  FakeCameraAccess first_camera(first_store);
  CaptureSessionManager first(planner, pose, quality, first_camera, *sensor, *first_store,
                              *projects, clock);

  ASSERT_TRUE(first.Begin(kProject, Spec()).ok());
  const NodeId node = first.GetPlan().value.nodes.front().id;
  ASSERT_TRUE(FireBurstOn(first, clock, node, BurstSpec{}).ok());
  auto before = first.Candidates(node);
  ASSERT_TRUE(before.ok()) << before.status.detail;
  ASSERT_FALSE(before.value.empty());
  ASSERT_TRUE(first.End().ok());

  auto second_store = NewStore();
  FakeCameraAccess second_camera(second_store);
  CaptureSessionManager second(planner, pose, quality, second_camera, *sensor, *second_store,
                               *projects, clock);

  auto resumed = second.Resume(kProject);
  ASSERT_TRUE(resumed.ok()) << resumed.status.detail;

  auto after = second.Candidates(node);
  ASSERT_TRUE(after.ok()) << after.status.detail;
  ASSERT_EQ(after.value.size(), before.value.size());
  EXPECT_EQ(after.value.front().id.value, before.value.front().id.value);
  EXPECT_EQ(after.value.front().frame.id.value, before.value.front().frame.id.value);
  // Ranked order, not the order they were written down in: `Candidates` promises best-first, and
  // a restore that hands back its own document order would quietly replace the engine's answer.
  for (size_t i = 0; i < after.value.size(); ++i) {
    EXPECT_EQ(after.value[i].id.value, before.value[i].id.value) << "candidate " << i;
  }

  // The bytes, not only the paperwork. This is the assertion the whole feature is for: the frame
  // was allocated by a store that no longer exists, and pinning it here has to fault it in from
  // the sink under the identity the document carried.
  const FrameRef& frame = after.value.front().frame;
  auto pinned = second_store->Pin(frame);
  ASSERT_TRUE(pinned.ok()) << pinned.status.detail;
  EXPECT_EQ(pinned.value.size(), static_cast<size_t>(frame.stride) * frame.height);
  EXPECT_TRUE(second_store->Release(frame).ok());
}

TEST_F(ResumedSession, ComesBackFromASessionThatWasNeverEnded) {
  // The case the feature exists for, and the one a document written only by End would miss: a
  // tab evicted while the phone is still being pointed around never reaches End at all. What is
  // on disk has to be whatever was true at the last cell, not whatever a clean exit wrote.
  auto first_store = NewStore();
  FakeCameraAccess first_camera(first_store);
  CaptureSessionManager first(planner, pose, quality, first_camera, *sensor, *first_store,
                              *projects, clock);
  ASSERT_TRUE(first.Begin(kProject, Spec()).ok());
  const NodeId node = first.GetPlan().value.nodes.front().id;
  ASSERT_TRUE(FireBurstOn(first, clock, node, BurstSpec{}).ok());
  const size_t captured = first.Candidates(node).value.size();
  ASSERT_GT(captured, 0u);
  // And now the tab goes away. No End, no close, no chance to tidy up.

  auto second_store = NewStore();
  FakeCameraAccess second_camera(second_store);
  CaptureSessionManager second(planner, pose, quality, second_camera, *sensor, *second_store,
                               *projects, clock);
  ASSERT_TRUE(second.Resume(kProject).ok());
  EXPECT_EQ(second.Candidates(node).value.size(), captured);
}

TEST_F(ResumedSession, DoesNotBringBackAFrameTheSessionNeverOwned) {
  // An offered frame is the caller's handle — a file import, a manual shutter — and this session
  // never spilled it, because cooling deliberately touches only its own (ADR 0023). So there is
  // nothing in the sink under that name, and writing it into the document would restore a
  // candidate whose first Pin fails: a cell claiming evidence it cannot produce. The handle went
  // away with the tab that owned it, and the honest restore is the one that says so by not
  // holding it.
  auto first_store = NewStore();
  FakeCameraAccess first_camera(first_store);
  CaptureSessionManager first(planner, pose, quality, first_camera, *sensor, *first_store,
                              *projects, clock);
  ASSERT_TRUE(first.Begin(kProject, Spec()).ok());
  const NodeId node = first.GetPlan().value.nodes.front().id;
  ASSERT_TRUE(FireBurstOn(first, clock, node, BurstSpec{}).ok());
  const size_t captured = first.Candidates(node).value.size();

  auto imported = first_store->Allocate(8, 8, PixelFormat::RGBA8);
  ASSERT_TRUE(imported.ok());
  ASSERT_TRUE(first.OfferFrame(node, imported.value, PoseSample{}).ok());
  ASSERT_EQ(first.Candidates(node).value.size(), captured + 1);
  ASSERT_TRUE(first.End().ok());

  auto second_store = NewStore();
  FakeCameraAccess second_camera(second_store);
  CaptureSessionManager second(planner, pose, quality, second_camera, *sensor, *second_store,
                               *projects, clock);
  ASSERT_TRUE(second.Resume(kProject).ok());

  auto restored = second.Candidates(node);
  ASSERT_TRUE(restored.ok()) << restored.status.detail;
  EXPECT_EQ(restored.value.size(), captured);
  // The property underneath the count: every candidate a resumed cell holds can produce its
  // pixels. A restored cell that cannot is the failure this whole feature exists to avoid.
  for (const Candidate& candidate : restored.value) {
    auto pinned = second_store->Pin(candidate.frame);
    EXPECT_TRUE(pinned.ok()) << "candidate " << candidate.id.value << ": " << pinned.status.detail;
    if (pinned.ok()) {
      EXPECT_TRUE(second_store->Release(candidate.frame).ok());
    }
  }
}

TEST_F(ResumedSession, ComesBackToTheSamePlanTheSessionWasCapturedAgainst) {
  // Node ids are indices into a tessellation, so a resumed session that replanned from whatever
  // the camera reports today would hand every restored candidate to a different cell if the
  // phone came back in another orientation. The plan the candidates were captured against is
  // part of the session, and it is stored rather than recomputed from the lens.
  // The real tessellation, not the null one: what is under test is that the plan came from the
  // stored field of view, and a planner that ignores the field of view cannot tell the two apart.
  RingsCoveragePlannerEngine rings;
  auto first_store = NewStore();
  FakeCameraAccess first_camera(first_store);
  CaptureSessionManager first(rings, pose, quality, first_camera, *sensor, *first_store,
                              *projects, clock);
  CapturePlanSpec spec = Spec();
  spec.acceptanceConeDeg = 7.5;
  ASSERT_TRUE(first.Begin(kProject, spec).ok());
  const CapturePlan planned = first.GetPlan().value;
  ASSERT_TRUE(first.End().ok());

  auto second_store = NewStore();
  FakeCameraAccess second_camera(second_store);
  // A different lens on the way back — a much wider field of view, which is the input the
  // tessellation is actually made from. A Resume that replanned from the camera in front of it
  // would come back with a coarser sphere and hand every restored candidate to another cell.
  CameraCapabilities wider = second_camera.Capabilities();
  wider.horizontalFovDeg = 110.0;
  wider.verticalFovDeg = 90.0;
  second_camera.SetCapabilities(wider);
  CaptureSessionManager second(rings, pose, quality, second_camera, *sensor, *second_store,
                               *projects, clock);
  ASSERT_TRUE(second.Resume(kProject).ok());

  auto plan = second.GetPlan();
  ASSERT_TRUE(plan.ok()) << plan.status.detail;
  ASSERT_EQ(plan.value.nodes.size(), planned.nodes.size());
  for (size_t i = 0; i < plan.value.nodes.size(); ++i) {
    EXPECT_EQ(plan.value.nodes[i].id.value, planned.nodes[i].id.value) << "node " << i;
    EXPECT_DOUBLE_EQ(plan.value.nodes[i].acceptanceConeDeg, planned.nodes[i].acceptanceConeDeg);
  }
}

// A store that takes a stated number of frames back and then refuses. Everything else is the
// real store's: what is under test is what the *manager* does with a refusal partway through, and
// a hand-written stub would have had to reimplement the tiering to get there.
class StubbornStore final : public IFrameStoreAccess {
 public:
  StubbornStore(std::shared_ptr<IFrameStoreAccess> inner, int adoptions)
      : inner_(std::move(inner)), adoptions_(adoptions) {}

  Result<FrameStoreBudget> Budget() override { return inner_->Budget(); }
  Result<FrameRef> Allocate(int32_t w, int32_t h, PixelFormat f) override {
    return inner_->Allocate(w, h, f);
  }
  Result<std::span<uint8_t>> Pin(const FrameRef& f) override { return inner_->Pin(f); }
  Status Release(const FrameRef& f) override { return inner_->Release(f); }
  Result<Residency> ResidencyOf(const FrameRef& f) override { return inner_->ResidencyOf(f); }
  Status Demote(const FrameRef& f, Residency t) override { return inner_->Demote(f, t); }
  Status Forget(const FrameRef& f) override { return inner_->Forget(f); }
  Result<uint64_t> ContentHash(const FrameRef& f) override { return inner_->ContentHash(f); }

  Status Adopt(const FrameRef& frame) override {
    if (adoptions_-- <= 0) return Fail(StatusCode::Internal, "test", "not taking any more");
    return inner_->Adopt(frame);
  }

 private:
  std::shared_ptr<IFrameStoreAccess> inner_;
  int adoptions_;
};

bool second_store_pin(IFrameStoreAccess& store, const FrameRef& frame) {
  auto pinned = store.Pin(frame);
  if (!pinned.ok()) return false;
  return store.Release(frame).ok();
}

// A pose engine that will not start, so a resume can be made to fail *after* it has taken the
// frames back. Everything before that point has already happened by then, which is the state the
// test below is about.
class UnwillingPoseEngine final : public IPoseEngine {
 public:
  Result<PoseState> Initial(PoseMode, MotionCapability) override {
    return Err<PoseState>(StatusCode::SensorUnavailable, "test", "not starting today");
  }
  Result<PoseState> Integrate(const PoseState&, std::span<const ImuSample>) override {
    return Err<PoseState>(StatusCode::SensorUnavailable, "test", "not starting today");
  }
  Result<PoseSample> Correct(const FrameRef&, const FrameRef&, const PoseSample&) override {
    return Err<PoseSample>(StatusCode::SensorUnavailable, "test", "not starting today");
  }
  Result<double> Stability(std::span<const ImuSample>) override {
    return Err<double>(StatusCode::SensorUnavailable, "test", "not starting today");
  }
};

TEST_F(ResumedSession, AFailedRestoreLeavesTheCaptureWhereItWas) {
  // The frames a resume adopts are not the resume's to throw away. They are the capture, they are
  // still named by the document on disk, and `Forget` takes the sink's copy with it — so undoing
  // a half-finished restore by forgetting what it had taken would delete the user's sphere to
  // tidy up after a failure, and every later attempt would pin-fail against an empty file.
  //
  // Nothing is undone, and nothing needs to be: a frame the store has taken back under the
  // identity the document gave it is exactly the frame the next attempt asks for.
  auto first_store = NewStore();
  FakeCameraAccess first_camera(first_store);
  CaptureSessionManager first(planner, pose, quality, first_camera, *sensor, *first_store,
                              *projects, clock);
  ASSERT_TRUE(first.Begin(kProject, Spec()).ok());
  const NodeId node = first.GetPlan().value.nodes.front().id;
  BurstSpec burst;
  burst.frameCount = 3;
  ASSERT_TRUE(FireBurstOn(first, clock, node, burst).ok());
  ASSERT_EQ(first.Candidates(node).value.size(), 3u);
  ASSERT_TRUE(first.End().ok());

  auto inner = NewStore();
  {
    StubbornStore stubborn(inner, 2);   // it takes two of the three, then stops
    FakeCameraAccess failing_camera(inner);
    CaptureSessionManager attempt(planner, pose, quality, failing_camera, *sensor, stubborn,
                                  *projects, clock);
    EXPECT_FALSE(attempt.Resume(kProject).ok());
  }

  // Straight into a second attempt, against the same store — which is what a page retrying gets,
  // since the store lives as long as the worker does.
  FakeCameraAccess second_camera(inner);
  CaptureSessionManager second(planner, pose, quality, second_camera, *sensor, *inner,
                               *projects, clock);
  ASSERT_TRUE(second.Resume(kProject).ok());

  auto restored = second.Candidates(node);
  ASSERT_TRUE(restored.ok()) << restored.status.detail;
  EXPECT_EQ(restored.value.size(), 3u);
  for (const Candidate& candidate : restored.value) {
    auto pinned = second_store_pin(*inner, candidate.frame);
    EXPECT_TRUE(pinned) << "candidate " << candidate.id.value;
  }
}

TEST_F(ResumedSession, AResumeThatFailsAfterTakingTheFramesCanStillBeTriedAgain) {
  // The same property one step later: everything is adopted and then the pose engine refuses. The
  // frames are in the store by that point, and an attempt that walked them back would hit exactly
  // the destructive path above — while one that leaves them makes the retry cheaper, not broken.
  auto first_store = NewStore();
  FakeCameraAccess first_camera(first_store);
  CaptureSessionManager first(planner, pose, quality, first_camera, *sensor, *first_store,
                              *projects, clock);
  ASSERT_TRUE(first.Begin(kProject, Spec()).ok());
  const NodeId node = first.GetPlan().value.nodes.front().id;
  ASSERT_TRUE(FireBurstOn(first, clock, node, BurstSpec{}).ok());
  ASSERT_TRUE(first.End().ok());

  auto inner = NewStore();
  UnwillingPoseEngine unwilling;
  {
    FakeCameraAccess sulking(inner);
    CaptureSessionManager attempt(planner, unwilling, quality, sulking, *sensor, *inner,
                                  *projects, clock);
    EXPECT_EQ(attempt.Resume(kProject).status.code, StatusCode::SensorUnavailable);
  }

  FakeCameraAccess second_camera(inner);
  CaptureSessionManager second(planner, pose, quality, second_camera, *sensor, *inner,
                               *projects, clock);
  EXPECT_TRUE(second.Resume(kProject).ok());
}

TEST_F(ResumedSession, RefusesADocumentFromAShapeThisBuildCannotRead) {
  // A half-read document is a coverage map missing the cells whose lines did not parse, which
  // says "captured" about a sphere with holes in it. Refusing is the only honest answer, and the
  // document is kept rather than deleted: the bytes it names may still be in the sink.
  // A real document with a newer version stamped on it, rather than a stub: a document that is
  // malformed anyway would be refused by the parse and prove nothing about the version line.
  auto first_store = NewStore();
  FakeCameraAccess first_camera(first_store);
  CaptureSessionManager first(planner, pose, quality, first_camera, *sensor, *first_store,
                              *projects, clock);
  ASSERT_TRUE(first.Begin(kProject, Spec()).ok());
  ASSERT_TRUE(FireBurstOn(first, clock, first.GetPlan().value.nodes.front().id, BurstSpec{}).ok());
  ASSERT_TRUE(first.End().ok());

  auto written = projects->ReadDocument(kProject, "session");
  ASSERT_TRUE(written.ok());
  const std::string stamp = "sphanorama-session 1";
  std::string newer = written.value;
  ASSERT_EQ(newer.rfind(stamp, 0), 0u);
  newer.replace(0, stamp.size(), "sphanorama-session 2");
  ASSERT_TRUE(projects->WriteDocument(kProject, "session", newer).ok());

  auto store_with_sink = NewStore();
  FakeCameraAccess fresh_camera(store_with_sink);
  CaptureSessionManager manager_(planner, pose, quality, fresh_camera, *sensor, *store_with_sink,
                                 *projects, clock);

  EXPECT_EQ(manager_.Resume(kProject).status.code, StatusCode::Unsupported);
  EXPECT_FALSE(fresh_camera.IsOpen());
  EXPECT_TRUE(projects->ReadDocument(kProject, "session").ok());
}

TEST_F(ResumedSession, RefusesADocumentNamingACellThePlanDoesNotHave) {
  // It should be unreachable, since the plan is made from the spec the document carries. But a
  // candidate filed under a cell the sphere does not have is one `Coverage` counts and nothing
  // can ever aim at — a capture that can never finish, with no way to see why.
  auto first_store = NewStore();
  FakeCameraAccess first_camera(first_store);
  CaptureSessionManager first(planner, pose, quality, first_camera, *sensor, *first_store,
                              *projects, clock);
  ASSERT_TRUE(first.Begin(kProject, Spec()).ok());
  ASSERT_TRUE(FireBurstOn(first, clock, first.GetPlan().value.nodes.front().id, BurstSpec{}).ok());
  ASSERT_TRUE(first.End().ok());

  auto document = projects->ReadDocument(kProject, "session");
  ASSERT_TRUE(document.ok());
  // Move one candidate to a cell number no tessellation this size reaches.
  std::string tampered = document.value;
  const size_t at = tampered.find("candidate ");
  ASSERT_NE(at, std::string::npos);
  const size_t idEnd = tampered.find(' ', tampered.find(' ', at) + 1);
  tampered.replace(idEnd + 1, tampered.find(' ', idEnd + 1) - idEnd - 1, "99999");
  ASSERT_TRUE(projects->WriteDocument(kProject, "session", tampered).ok());

  auto second_store = NewStore();
  FakeCameraAccess second_camera(second_store);
  CaptureSessionManager second(planner, pose, quality, second_camera, *sensor, *second_store,
                               *projects, clock);
  EXPECT_FALSE(second.Resume(kProject).ok());
}

TEST_F(ResumedSession, RefusesAProjectThatWasNeverCaptured) {
  // A project with a title and no session document is one that was created and never begun.
  // Resuming it has to say so rather than handing back an empty session, which would read as a
  // capture whose cells had all been lost.
  auto store_with_sink = NewStore();
  FakeCameraAccess fresh_camera(store_with_sink);
  CaptureSessionManager manager_(planner, pose, quality, fresh_camera, *sensor, *store_with_sink,
                                 *projects, clock);
  auto resumed = manager_.Resume(kProject);
  EXPECT_EQ(resumed.status.code, StatusCode::NotFound);
  // And it must not have opened the camera to find that out.
  EXPECT_FALSE(fresh_camera.IsOpen());
}

TEST_F(ResumedSession, WillNotResumeOverALiveSession) {
  // The same rule Begin has, for the same reason: replacing a live session drops its candidate
  // map with the frames still in the store and nothing left holding the handles.
  auto store_with_sink = NewStore();
  FakeCameraAccess fresh_camera(store_with_sink);
  CaptureSessionManager manager_(planner, pose, quality, fresh_camera, *sensor, *store_with_sink,
                                 *projects, clock);
  ASSERT_TRUE(manager_.Begin(kProject, Spec()).ok());
  auto resumed = manager_.Resume(kProject);
  EXPECT_EQ(resumed.status.code, StatusCode::FailedPrecondition);
}

}  // namespace
}  // namespace sphanorama
