// The build manager's sequencing, against engines that answer as told (ADR 0070).
//
// The engines here do no registration and no composition: what is under test is which frames the
// manager hands them, in what order, one step at a time, and what it gives back on every way out.
// `panorama_build_accuracy_test.cpp` runs the same manager over the real engines and a rendered
// ring, which is where whether the answer is *right* is asked.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "managers/panorama_build_manager/panorama_build_manager.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "support/fake_project_store_access.h"
#include "support/fake_spill_sink.h"
#include "utilities/camera_model.h"
#include "utilities/quaternion.h"
#include "utilities/session_document.h"

namespace sphanorama {
namespace {

constexpr int32_t kWidth = 64;
constexpr int32_t kHeight = 48;
constexpr double kHorizontalFovDeg = 66.0;
constexpr double kVerticalFovDeg = 50.0;
constexpr ProjectId kProject{7};

// Answers as told, and writes down what it was asked.
//
// Extraction pins and releases the frame it is handed, as the real engine does, so a spilled
// frame comes back resident — the cost the manager is responsible for undoing. Each feature set
// is two real frames in the store, so a set the manager fails to forget is a leak the store's
// budget shows.
class ScriptedRegistrationEngine final : public IRegistrationEngine {
 public:
  explicit ScriptedRegistrationEngine(IFrameStoreAccess& store) : store_(store) {}

  Result<FeatureSet> ExtractFeatures(const FrameRef& frame) override {
    extracted.push_back(frame.id);
    // The real engine pins before it allocates, so a refusal for want of room arrives with the
    // frame already faulted in; `failAfterReading` says which way round this one refuses.
    const auto found = extractFailures.find(frame.id.value);
    if (found != extractFailures.end() && !failAfterReading) return found->second;
    auto pinned = store_.Pin(frame);
    if (!pinned.ok()) return pinned.status;
    if (auto released = store_.Release(frame); !released.ok()) return released;
    if (found != extractFailures.end()) return found->second;
    FeatureSet set;
    set.frame = frame.id;
    set.extractor = 1;
    if (featureless.count(frame.id.value) != 0) return Ok(set);
    auto descriptors = store_.Allocate(8, 4, PixelFormat::Gray8);
    if (!descriptors.ok()) return descriptors.status;
    auto keypoints = store_.Allocate(8, 4, PixelFormat::Gray8);
    if (!keypoints.ok()) return keypoints.status;
    set.count = 4;
    set.descriptors = descriptors.value;
    set.keypoints = keypoints.value;
    return Ok(set);
  }

  Result<PairwiseResult> EstimatePairwise(const FeatureSet& a, const FeatureSet& b,
                                          const Quat& prior, const Intrinsics& lens) override {
    asked.push_back({a.frame, b.frame, prior, lens});
    // The real engine's `BorrowedFrame` discards a refused `Release`, which leaves the set's frame
    // pinned behind a call that answered.
    if (leaveFeaturesPinned) (void)store_.Pin(a.descriptors);
    if (auto found = pairFailures.find({a.frame.value, b.frame.value});
        found != pairFailures.end()) {
      return found->second;
    }
    PairwiseResult pair;
    pair.a = a.frame;
    pair.b = b.frame;
    pair.relativeRotation = prior;
    pair.inliers = 50;
    pair.correspondences = 60;
    pair.accepted = unaccepted.count({a.frame.value, b.frame.value}) == 0;
    return Ok(pair);
  }

  Result<GlobalSolution> Refine(std::span<const PairwiseResult> pairs,
                                std::span<const FramePrior> priors,
                                const Intrinsics& initial) override {
    refinedPairs.assign(pairs.begin(), pairs.end());
    refinedPriors.assign(priors.begin(), priors.end());
    refinedLens = initial;
    ++refines;
    if (!refineFailure.ok()) return refineFailure;
    GlobalSolution solution;
    solution.intrinsics = initial;
    for (const FramePrior& prior : priors) {
      if (dropped.count(prior.frame.value) != 0) {
        solution.droppedFrames.push_back(prior.frame);
        continue;
      }
      solution.frames.push_back(prior.frame);
      solution.rotations.push_back(prior.pose.orientation);
    }
    // Reversed when asked, so a manager that hands the compositor its own frame order rather than
    // the solution's is caught.
    if (reverse) {
      std::reverse(solution.frames.begin(), solution.frames.end());
      std::reverse(solution.rotations.begin(), solution.rotations.end());
    }
    if (placeAStranger) {
      solution.frames.push_back(FrameId{999999});
      solution.rotations.push_back(Quat{});
    }
    return Ok(solution);
  }

  struct Asked {
    FrameId a, b;
    Quat prior;
    Intrinsics lens;
  };

  std::vector<FrameId> extracted;
  std::vector<Asked> asked;
  std::vector<PairwiseResult> refinedPairs;
  std::vector<FramePrior> refinedPriors;
  Intrinsics refinedLens;
  int refines = 0;
  std::map<uint64_t, Status> extractFailures;
  bool failAfterReading = false;
  std::set<uint64_t> featureless;
  std::map<std::pair<uint64_t, uint64_t>, Status> pairFailures;
  Status refineFailure;
  std::set<uint64_t> dropped;
  bool reverse = false;
  bool placeAStranger = false;
  bool leaveFeaturesPinned = false;
  std::set<std::pair<uint64_t, uint64_t>> unaccepted;

 private:
  IFrameStoreAccess& store_;
};

class ScriptedCompositionEngine final : public ICompositionEngine {
 public:
  explicit ScriptedCompositionEngine(IFrameStoreAccess& store) : store_(store) {}

  Result<GainMap> CompensateExposure(const GlobalSolution&, std::span<const FrameRef>) override {
    return Err<GainMap>(StatusCode::Unsupported, "test");
  }
  Result<GhostReport> DetectGhosts(const GlobalSolution&, std::span<const Candidate>) override {
    return Err<GhostReport>(StatusCode::Unsupported, "test");
  }
  Result<SeamMap> FindSeams(const GlobalSolution&, std::span<const FrameRef>, const GainMap&,
                            const GhostReport&, const BuildSpec&) override {
    return Err<SeamMap>(StatusCode::Unsupported, "test");
  }
  Result<FrameRef> BlendTile(const GlobalSolution&, std::span<const FrameRef>, const GainMap&,
                             const SeamMap&, const BuildSpec&, int32_t, int32_t) override {
    return Err<FrameRef>(StatusCode::Unsupported, "test");
  }

  Result<FrameRef> RenderPreview(const GlobalSolution& solution, std::span<const FrameRef> frames,
                                 const GainMap&, int32_t maxWidth) override {
    ++renders;
    renderedSolution = solution;
    renderedFrames.assign(frames.begin(), frames.end());
    renderedWidth = maxWidth;
    // The contract's other hard refusal: a frame handed in that the store would not release is left
    // pinned by the call, and out of its tier.
    if (leaveInputsPinned) {
      for (const FrameRef& frame : frames) (void)store_.Pin(frame);
      return failure;
    }
    auto answer = store_.Allocate(maxWidth, maxWidth / 2, PixelFormat::RGBA8);
    if (!answer.ok()) return answer.status;
    if (!failure.ok()) {
      // The contract's hardest refusal: the answer could not be taken back, so it comes back in
      // the value, pinned, for the caller to release and forget.
      if (handBackPinned) {
        (void)store_.Pin(answer.value);
        return Result<FrameRef>{failure, answer.value};
      }
      (void)store_.Forget(answer.value);
      return failure;
    }
    return answer;
  }

  int renders = 0;
  GlobalSolution renderedSolution;
  std::vector<FrameRef> renderedFrames;
  int32_t renderedWidth = 0;
  Status failure;
  bool handBackPinned = false;
  bool leaveInputsPinned = false;

 private:
  IFrameStoreAccess& store_;
};

// The store as the manager sees it, refusing `Release` as many times as it is told to. No real
// store refuses a release of a frame it holds pinned, and `RenderPreview` hands its answer back in
// exactly the case where one did — so the manager's retry is reachable only through this.
class ReluctantFrameStore final : public IFrameStoreAccess {
 public:
  explicit ReluctantFrameStore(IFrameStoreAccess& real) : real_(real) {}

  Result<FrameStoreBudget> Budget() override { return real_.Budget(); }
  Result<FrameRef> Allocate(int32_t width, int32_t height, PixelFormat format) override {
    return real_.Allocate(width, height, format);
  }
  Result<std::span<uint8_t>> Pin(const FrameRef& frame) override {
    if (pinRefusals > 0) {
      --pinRefusals;
      return Err<std::span<uint8_t>>(StatusCode::Internal, "test", "will not fault it in this time");
    }
    return real_.Pin(frame);
  }
  Status Release(const FrameRef& frame) override {
    if (releaseRefusals > 0) {
      --releaseRefusals;
      return Fail(StatusCode::Internal, "test", "will not release it this time");
    }
    return real_.Release(frame);
  }
  Result<Residency> ResidencyOf(const FrameRef& frame) override {
    if (residencyAnswersFirst > 0) {
      --residencyAnswersFirst;
    } else if (residencyRefusals > 0) {
      --residencyRefusals;
      return Err<Residency>(StatusCode::Internal, "test", "cannot say where it is");
    }
    return real_.ResidencyOf(frame);
  }
  Status Demote(const FrameRef& frame, Residency target) override {
    if (demoteRefusals > 0) {
      --demoteRefusals;
      return Fail(StatusCode::StorageQuotaExceeded, "test", "no room in the tier");
    }
    return real_.Demote(frame, target);
  }
  Status Adopt(const FrameRef& frame) override { return real_.Adopt(frame); }
  Status Forget(const FrameRef& frame) override {
    if (forgetRefusals > 0) {
      --forgetRefusals;
      return Fail(StatusCode::Internal, "test", "will not forget it this time");
    }
    return real_.Forget(frame);
  }
  Status Clear() override { return real_.Clear(); }
  Result<uint64_t> TierGeneration() override { return real_.TierGeneration(); }
  Result<uint64_t> ContentHash(const FrameRef& frame) override { return real_.ContentHash(frame); }

  int pinRefusals = 0;
  int releaseRefusals = 0;
  int residencyRefusals = 0;
  // Answered truthfully before `residencyRefusals` begin, so a refusal can be aimed at one read.
  int residencyAnswersFirst = 0;
  int demoteRefusals = 0;
  int forgetRefusals = 0;

 private:
  IFrameStoreAccess& real_;
};

// A project store that fails one key's reads with `StorageQuotaExceeded`. No store here fails a
// read with anything but `NotFound`, and the manager must not read any other failure as absence —
// of a pick as "nobody chose", of a title as "no such project".
class UnreadableDocumentStore final : public IProjectStoreAccess {
 public:
  UnreadableDocumentStore(IProjectStoreAccess& real, std::string key) : real_(real), key_(key) {}
  Result<std::vector<ProjectId>> ListProjects() override { return real_.ListProjects(); }
  Result<std::string> ReadDocument(ProjectId project, std::string_view key) override {
    if (key == key_) return Err<std::string>(StatusCode::StorageQuotaExceeded, "test", "unreadable");
    return real_.ReadDocument(project, key);
  }
  Status WriteDocument(ProjectId project, std::string_view key, std::string_view value) override {
    return real_.WriteDocument(project, key, value);
  }
  Status DeleteProject(ProjectId project) override { return real_.DeleteProject(project); }

 private:
  IProjectStoreAccess& real_;
  std::string key_;
};

class PanoramaBuildManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(projects_.WriteDocument(kProject, "title", "a hangar").ok());
  }

  // A ring of `count` cells thirty degrees apart about the vertical, one candidate each, written
  // into the project's session document as a capture would have left it.
  void CaptureRing(int count) {
    for (int i = 0; i < count; ++i) {
      AddCandidate(NodeId{static_cast<uint64_t>(i + 1)},
                   FromAzimuthElevation(30.0 * i, 0.0));
    }
    WriteDocument();
  }

  Candidate AddCandidate(NodeId node, Quat orientation, int32_t width = kWidth,
                         int32_t height = kHeight) {
    auto frame = store_.Allocate(width, height, PixelFormat::RGBA8);
    EXPECT_TRUE(frame.ok());
    Candidate candidate;
    candidate.id = CandidateId{next_candidate_++};
    candidate.node = node;
    candidate.frame = frame.value;
    candidate.pose.orientation = orientation;
    candidate.pose.confidence = 1.0;
    document_.candidates.push_back(candidate);
    return candidate;
  }

  void WriteDocument() {
    auto generation = store_.TierGeneration();
    ASSERT_TRUE(generation.ok());
    document_.session = 1;
    document_.nextCandidate = next_candidate_;
    document_.generation = generation.value;
    document_.spec.horizontalFovDeg = kHorizontalFovDeg;
    document_.spec.verticalFovDeg = kVerticalFovDeg;
    // The camera's largest size, which is not the size the frames were grabbed at.
    document_.lens.width = 4000;
    document_.lens.height = 3000;
    ASSERT_TRUE(projects_.WriteDocument(kProject, kSessionDocumentKey,
                                        EncodeSessionDocument(document_)).ok());
  }

  int64_t HeapUsed() {
    auto budget = store_.Budget();
    EXPECT_TRUE(budget.ok());
    return budget.value.heapUsedBytes;
  }

  // Polls until the build finishes, and answers every progress it reported on the way.
  std::vector<BuildProgress> RunToEnd(BuildId build) { return RunToEnd(manager_, build); }

  // Never empty: a build that could not be polled at all answers one progress saying so, so a
  // caller reading `.back()` reads a failure rather than past the end.
  static std::vector<BuildProgress> RunToEnd(IPanoramaBuildManager& manager, BuildId build) {
    std::vector<BuildProgress> seen;
    for (int i = 0; i < 1000; ++i) {
      auto progress = manager.Poll(build);
      EXPECT_TRUE(progress.ok()) << progress.status.detail;
      if (!progress.ok()) {
        BuildProgress unpolled;
        unpolled.stage = BuildStage::Failed;
        unpolled.failure = progress.status;
        seen.push_back(unpolled);
        break;
      }
      seen.push_back(progress.value);
      if (progress.value.stage == BuildStage::Complete
          || progress.value.stage == BuildStage::Failed) {
        break;
      }
    }
    return seen;
  }

  FakeSpillSink sink_;
  MemoryFrameStoreAccess store_{1 << 26, &sink_};
  FakeProjectStoreAccess projects_;
  ScriptedRegistrationEngine registration_{store_};
  ScriptedCompositionEngine composition_{store_};
  PanoramaBuildManager manager_{registration_, composition_, store_, projects_};
  SessionDocument document_;
  uint64_t next_candidate_ = 1;
};

// ------------------------------------------------------------------ what Start refuses

TEST_F(PanoramaBuildManagerTest, StartRefusesAProjectThatDoesNotExist) {
  EXPECT_EQ(manager_.Start(ProjectId{99}, BuildSpec{}).status.code, StatusCode::NotFound);
}

// A capture under an id nobody created — the title is what makes a project one, as it is for
// `Begin`.
TEST_F(PanoramaBuildManagerTest, StartRefusesACaptureInAProjectNobodyCreated) {
  CaptureRing(2);
  ASSERT_TRUE(projects_.WriteDocument(ProjectId{99}, kSessionDocumentKey,
                                      EncodeSessionDocument(document_)).ok());
  const auto started = manager_.Start(ProjectId{99}, BuildSpec{});
  EXPECT_EQ(started.status.code, StatusCode::NotFound);
  EXPECT_EQ(started.status.detail, "project 99 does not exist");
}

// With no spill tier, frame identities restart with every tab and a stale document matches a live
// capture's frames by id alone.
TEST_F(PanoramaBuildManagerTest, StartRefusesAStoreWithNoSpillTier) {
  MemoryFrameStoreAccess tierless{1 << 26};
  PanoramaBuildManager manager{registration_, composition_, tierless, projects_};
  auto frame = tierless.Allocate(kWidth, kHeight, PixelFormat::RGBA8);
  ASSERT_TRUE(frame.ok());
  Candidate candidate;
  candidate.id = CandidateId{1};
  candidate.node = NodeId{1};
  candidate.frame = frame.value;
  candidate.pose.confidence = 1.0;
  document_.candidates.push_back(candidate);
  document_.generation = 0;
  document_.spec.horizontalFovDeg = kHorizontalFovDeg;
  document_.spec.verticalFovDeg = kVerticalFovDeg;
  document_.session = 1;
  document_.nextCandidate = 2;
  ASSERT_TRUE(projects_.WriteDocument(kProject, kSessionDocumentKey,
                                      EncodeSessionDocument(document_)).ok());
  EXPECT_EQ(manager.Start(kProject, BuildSpec{}).status.code, StatusCode::FailedPrecondition);
  EXPECT_TRUE(tierless.Forget(frame.value).ok());
}

// Each would reach the solve, which refuses it, after every extraction and pair had been paid for.
TEST_F(PanoramaBuildManagerTest, StartRefusesOneFrameInTwoCells) {
  CaptureRing(3);
  document_.candidates[2].frame = document_.candidates[0].frame;
  WriteDocument();
  EXPECT_EQ(manager_.Start(kProject, BuildSpec{}).status.code, StatusCode::FailedPrecondition);
}

TEST_F(PanoramaBuildManagerTest, StartRefusesACaptureNoMeasuredPoseAnchors) {
  CaptureRing(3);
  for (Candidate& candidate : document_.candidates) candidate.pose.confidence = 0.0;
  WriteDocument();
  EXPECT_EQ(manager_.Start(kProject, BuildSpec{}).status.code, StatusCode::FailedPrecondition);
  // One measured pose is enough to say which way the panorama faces.
  document_.candidates[1].pose.confidence = 1.0;
  WriteDocument();
  EXPECT_TRUE(manager_.Start(kProject, BuildSpec{}).ok());
}

TEST_F(PanoramaBuildManagerTest, StartRefusesAProjectWithNoCapture) {
  EXPECT_EQ(manager_.Start(kProject, BuildSpec{}).status.code, StatusCode::NotFound);
}

TEST_F(PanoramaBuildManagerTest, StartRefusesADocumentItCannotRead) {
  CaptureRing(3);
  ASSERT_TRUE(
      projects_.WriteDocument(kProject, kSessionDocumentKey, "sphanorama-session 3\n").ok());
  EXPECT_EQ(manager_.Start(kProject, BuildSpec{}).status.code, StatusCode::Unsupported);
}

TEST_F(PanoramaBuildManagerTest, StartRefusesACaptureWithNothingInIt) {
  WriteDocument();
  EXPECT_EQ(manager_.Start(kProject, BuildSpec{}).status.code, StatusCode::FailedPrecondition);
}

TEST_F(PanoramaBuildManagerTest, StartRefusesACaptureWhoseFramesTheStoreNoLongerHolds) {
  CaptureRing(3);
  ASSERT_TRUE(store_.Forget(document_.candidates[1].frame).ok());
  const auto started = manager_.Start(kProject, BuildSpec{});
  EXPECT_EQ(started.status.code, StatusCode::FailedPrecondition);
  EXPECT_TRUE(registration_.extracted.empty());
}

// Frame identities restart with every process and the tier outlives it, so a document whose tier
// is not the store's names frames that may be somebody else's (ADR 0035).
TEST_F(PanoramaBuildManagerTest, StartRefusesACaptureFromAnotherTier) {
  CaptureRing(3);
  document_.generation += 1;
  ASSERT_TRUE(projects_.WriteDocument(kProject, kSessionDocumentKey,
                                      EncodeSessionDocument(document_)).ok());
  EXPECT_EQ(manager_.Start(kProject, BuildSpec{}).status.code, StatusCode::FailedPrecondition);
}

TEST_F(PanoramaBuildManagerTest, StartRefusesFramesOfMoreThanOneSize) {
  // Each dimension alone, so neither half of the comparison is carried by the other.
  for (const auto& [width, height] : {std::pair<int32_t, int32_t>{kWidth * 2, kHeight},
                                      std::pair<int32_t, int32_t>{kWidth, kHeight * 2}}) {
    document_.candidates.clear();
    AddCandidate(NodeId{1}, FromAzimuthElevation(0.0, 0.0));
    AddCandidate(NodeId{2}, FromAzimuthElevation(30.0, 0.0), width, height);
    WriteDocument();
    EXPECT_EQ(manager_.Start(kProject, BuildSpec{}).status.code, StatusCode::FailedPrecondition)
        << width << "x" << height;
  }
}

TEST_F(PanoramaBuildManagerTest, StartRefusesAFieldOfViewThatIsNotALens) {
  AddCandidate(NodeId{1}, FromAzimuthElevation(0.0, 0.0));
  WriteDocument();
  document_.spec.horizontalFovDeg = 190.0;
  ASSERT_TRUE(projects_.WriteDocument(kProject, kSessionDocumentKey,
                                      EncodeSessionDocument(document_)).ok());
  EXPECT_EQ(manager_.Start(kProject, BuildSpec{}).status.code, StatusCode::FailedPrecondition);
}

TEST_F(PanoramaBuildManagerTest, StartRefusesACubemapAndAnOutputTooNarrowToDraw) {
  CaptureRing(3);
  BuildSpec cubemap;
  cubemap.projection = Projection::Cubemap;
  EXPECT_EQ(manager_.Start(kProject, cubemap).status.code, StatusCode::Unsupported);
  BuildSpec narrow;
  narrow.outputWidth = 1;
  EXPECT_EQ(manager_.Start(kProject, narrow).status.code, StatusCode::InvalidArgument);
  narrow.outputWidth = 2;
  EXPECT_TRUE(manager_.Start(kProject, narrow).ok());
}

TEST_F(PanoramaBuildManagerTest, StartRefusesWhileABuildIsRunning) {
  CaptureRing(3);
  auto first = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(manager_.Poll(first.value).ok());
  EXPECT_EQ(manager_.Start(kProject, BuildSpec{}).status.code, StatusCode::FailedPrecondition);
}

// ------------------------------------------------------------------ which frame a cell gives

TEST_F(PanoramaBuildManagerTest, ACellGivesItsRankedBestWhereNobodyChose) {
  const Candidate best = AddCandidate(NodeId{1}, FromAzimuthElevation(0.0, 0.0));
  AddCandidate(NodeId{1}, FromAzimuthElevation(0.5, 0.0));
  const Candidate other = AddCandidate(NodeId{2}, FromAzimuthElevation(30.0, 0.0));
  WriteDocument();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok()) << build.status.detail;
  RunToEnd(build.value);
  EXPECT_EQ(registration_.extracted, (std::vector<FrameId>{best.frame.id, other.frame.id}));
}

TEST_F(PanoramaBuildManagerTest, ACellGivesTheCandidateSomebodyChose) {
  AddCandidate(NodeId{1}, FromAzimuthElevation(0.0, 0.0));
  const Candidate chosen = AddCandidate(NodeId{1}, FromAzimuthElevation(0.5, 0.0));
  const Candidate other = AddCandidate(NodeId{2}, FromAzimuthElevation(30.0, 0.0));
  WriteDocument();
  ASSERT_TRUE(projects_.WriteDocument(kProject, SelectionDocumentKey(NodeId{1}),
                                      std::to_string(chosen.id.value)).ok());
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok()) << build.status.detail;
  RunToEnd(build.value);
  EXPECT_EQ(registration_.extracted, (std::vector<FrameId>{chosen.frame.id, other.frame.id}));
}

// A discarding retake takes the picked frame with it and leaves the pick behind, and no screen can
// show a frame that is gone — so the ranking decides again, rather than every later build being
// refused for a pick nobody can clear.
TEST_F(PanoramaBuildManagerTest, APickTheCellNoLongerHoldsGivesWayToTheRanking) {
  const Candidate best = AddCandidate(NodeId{1}, FromAzimuthElevation(0.0, 0.0));
  const Candidate elsewhere = AddCandidate(NodeId{2}, FromAzimuthElevation(30.0, 0.0));
  WriteDocument();
  ASSERT_TRUE(projects_.WriteDocument(kProject, SelectionDocumentKey(NodeId{1}),
                                      std::to_string(elsewhere.id.value)).ok());
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok()) << build.status.detail;
  RunToEnd(build.value);
  EXPECT_EQ(registration_.extracted, (std::vector<FrameId>{best.frame.id, elsewhere.frame.id}));
}

TEST_F(PanoramaBuildManagerTest, ATitleThatCannotBeReadIsNotANoSuchProject) {
  CaptureRing(2);
  UnreadableDocumentStore unreadable{projects_, "title"};
  PanoramaBuildManager manager{registration_, composition_, store_, unreadable};
  EXPECT_EQ(manager.Start(kProject, BuildSpec{}).status.code, StatusCode::StorageQuotaExceeded);
}

// A store that cannot say where a frame is has not said the frame is gone.
TEST_F(PanoramaBuildManagerTest, AStoreThatCannotSayWhereAFrameIsIsNotALostCapture) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(2);
  reluctant.residencyRefusals = 1;
  EXPECT_EQ(manager.Start(kProject, BuildSpec{}).status.code, StatusCode::Internal);
}

// A cell ranked with an unmeasured frame first gives its best measured one instead: a frame with no
// measured pose is paired with nothing, and the solve drops it, so choosing it would lose the cell.
TEST_F(PanoramaBuildManagerTest, ARankedCellGivesItsBestMeasuredFrame) {
  AddCandidate(NodeId{1}, FromAzimuthElevation(0.0, 0.0));
  document_.candidates.back().pose.confidence = 0.0;
  const Candidate measured = AddCandidate(NodeId{1}, FromAzimuthElevation(0.5, 0.0));
  const Candidate other = AddCandidate(NodeId{2}, FromAzimuthElevation(30.0, 0.0));
  WriteDocument();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok()) << build.status.detail;
  RunToEnd(build.value);
  EXPECT_EQ(registration_.extracted, (std::vector<FrameId>{measured.frame.id, other.frame.id}));
}

// A cell with no measured frame gives its ranked best all the same — and loses it to the solve.
TEST_F(PanoramaBuildManagerTest, ACellWithNoMeasuredFrameGivesItsRankedBest) {
  const Candidate best = AddCandidate(NodeId{1}, FromAzimuthElevation(0.0, 0.0));
  document_.candidates.back().pose.confidence = 0.0;
  AddCandidate(NodeId{1}, FromAzimuthElevation(0.5, 0.0));
  document_.candidates.back().pose.confidence = 0.0;
  const Candidate other = AddCandidate(NodeId{2}, FromAzimuthElevation(30.0, 0.0));
  WriteDocument();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok()) << build.status.detail;
  RunToEnd(build.value);
  EXPECT_EQ(registration_.extracted, (std::vector<FrameId>{best.frame.id, other.frame.id}));
}

TEST_F(PanoramaBuildManagerTest, ACaptureThatCannotBeReadIsNotAProjectWithNoCapture) {
  CaptureRing(2);
  UnreadableDocumentStore unreadable{projects_, kSessionDocumentKey};
  PanoramaBuildManager manager{registration_, composition_, store_, unreadable};
  EXPECT_EQ(manager.Start(kProject, BuildSpec{}).status.code, StatusCode::StorageQuotaExceeded);
}

// A pick the document never saw — one an earlier build's door recorded, or an edited one — names a
// frame it does not know, and building from the ranking would use a frame the strip shows as unpicked.
TEST_F(PanoramaBuildManagerTest, APickNewerThanTheDocumentRefusesTheBuild) {
  CaptureRing(2);
  ASSERT_TRUE(projects_.WriteDocument(kProject, SelectionDocumentKey(NodeId{1}),
                                      std::to_string(document_.nextCandidate)).ok());
  EXPECT_EQ(manager_.Start(kProject, BuildSpec{}).status.code, StatusCode::FailedPrecondition);
  EXPECT_TRUE(registration_.extracted.empty());
}

TEST_F(PanoramaBuildManagerTest, APickThatCannotBeReadRefusesTheBuild) {
  CaptureRing(2);
  UnreadableDocumentStore unreadable{projects_, SelectionDocumentKey(NodeId{2})};
  PanoramaBuildManager manager{registration_, composition_, store_, unreadable};
  EXPECT_EQ(manager.Start(kProject, BuildSpec{}).status.code, StatusCode::StorageQuotaExceeded);
}

TEST_F(PanoramaBuildManagerTest, APickThatIsNotACandidateIsRefused) {
  CaptureRing(2);
  ASSERT_TRUE(projects_.WriteDocument(kProject, SelectionDocumentKey(NodeId{1}), "one").ok());
  EXPECT_EQ(manager_.Start(kProject, BuildSpec{}).status.code, StatusCode::Internal);
}

// ------------------------------------------------------------------ one step a Poll

TEST_F(PanoramaBuildManagerTest, StartReadsNoPixels) {
  CaptureRing(12);
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok()) << build.status.detail;
  EXPECT_TRUE(registration_.extracted.empty());
  EXPECT_TRUE(registration_.asked.empty());
  EXPECT_EQ(registration_.refines, 0);
  EXPECT_EQ(composition_.renders, 0);
}

// Twelve extractions, twelve pairs, one solve and one preview, one a call, in that order — and a
// fraction that climbs every step and is one only at the end.
TEST_F(PanoramaBuildManagerTest, EachPollDoesOneStep) {
  CaptureRing(12);
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok()) << build.status.detail;

  std::vector<BuildStage> stages;
  double last = 0.0;
  for (int step = 0; step < 26; ++step) {
    const size_t extractedBefore = registration_.extracted.size();
    const size_t askedBefore = registration_.asked.size();
    const int work = registration_.refines + composition_.renders;
    auto progress = manager_.Poll(build.value);
    ASSERT_TRUE(progress.ok()) << progress.status.detail;
    const size_t did = (registration_.extracted.size() - extractedBefore)
                       + (registration_.asked.size() - askedBefore)
                       + static_cast<size_t>(registration_.refines + composition_.renders - work);
    EXPECT_EQ(did, 1u) << "step " << step;
    EXPECT_GT(progress.value.fraction, last) << "step " << step;
    last = progress.value.fraction;
    stages.push_back(progress.value.stage);
    if (step < 25) {
      EXPECT_LT(progress.value.fraction, 1.0) << "step " << step;
    }
  }
  // Each progress names the step the next `Poll` does.
  std::vector<BuildStage> expected(11, BuildStage::Features);
  expected.insert(expected.end(), 12, BuildStage::PairwiseMatching);
  expected.push_back(BuildStage::GlobalSolve);
  expected.push_back(BuildStage::Projecting);
  expected.push_back(BuildStage::Complete);
  EXPECT_EQ(stages, expected);
  EXPECT_EQ(last, 1.0);
  EXPECT_EQ(registration_.extracted.size(), 12u);
  EXPECT_EQ(registration_.asked.size(), 12u);
  EXPECT_EQ(registration_.refines, 1);
  EXPECT_EQ(composition_.renders, 1);
}

TEST_F(PanoramaBuildManagerTest, AFinishedBuildAnswersTheSameProgressAndDoesNothing) {
  CaptureRing(3);
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  const BuildProgress finished = RunToEnd(build.value).back();
  ASSERT_EQ(finished.stage, BuildStage::Complete);
  auto again = manager_.Poll(build.value);
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(again.value.stage, BuildStage::Complete);
  EXPECT_EQ(again.value.fraction, 1.0);
  EXPECT_EQ(composition_.renders, 1);
}

// ------------------------------------------------------------------ what each engine is handed

// On a ring thirty degrees apart, with a lens fifty degrees tall, each frame overlaps its two
// neighbours and no other — the ring's closing pair among them, which is the edge a chain throws
// away and a loop needs (ADR 0065).
TEST_F(PanoramaBuildManagerTest, PairsAreTheNeighboursAndTheClosingPair) {
  CaptureRing(12);
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  RunToEnd(build.value);

  std::set<std::pair<uint64_t, uint64_t>> pairs;
  for (const auto& asked : registration_.asked) {
    pairs.insert({std::min(asked.a.value, asked.b.value), std::max(asked.a.value, asked.b.value)});
  }
  std::set<std::pair<uint64_t, uint64_t>> neighbours;
  for (size_t i = 0; i < 12; ++i) {
    const uint64_t a = document_.candidates[i].frame.id.value;
    const uint64_t b = document_.candidates[(i + 1) % 12].frame.id.value;
    neighbours.insert({std::min(a, b), std::max(a, b)});
  }
  EXPECT_EQ(pairs, neighbours);
}

// The prior is how the capture's poses say the second frame sits relative to the first, in the
// convention the registration harness hands its priors in.
TEST_F(PanoramaBuildManagerTest, APairsPriorIsWhatThePosesSay) {
  CaptureRing(12);
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  RunToEnd(build.value);
  ASSERT_FALSE(registration_.asked.empty());
  std::map<uint64_t, Quat> poseOf;
  for (const Candidate& candidate : document_.candidates) {
    poseOf[candidate.frame.id.value] = candidate.pose.orientation;
  }
  for (const auto& asked : registration_.asked) {
    const Quat expected =
        Multiply(Conjugate(poseOf.at(asked.b.value)), poseOf.at(asked.a.value));
    EXPECT_LT(AngleBetween(asked.prior, expected), 1e-9);
    // Thirty degrees, so a prior built the other way round — the inverse — is not within reach.
    EXPECT_GT(AngleBetween(asked.prior, Conjugate(expected)), 0.5);
  }
}

// At the grabbed frames' size, not the camera's largest: matches are pixels of the grabbed frame.
TEST_F(PanoramaBuildManagerTest, TheLensIsTheCapturesFieldOfViewAtTheFramesSize) {
  CaptureRing(12);
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  RunToEnd(build.value);
  const Intrinsics expected =
      LensFromFieldOfView(kHorizontalFovDeg, kVerticalFovDeg, kWidth, kHeight);
  ASSERT_FALSE(registration_.asked.empty());
  for (const auto& asked : registration_.asked) {
    EXPECT_EQ(asked.lens.width, kWidth);
    EXPECT_EQ(asked.lens.fx, expected.fx);
    EXPECT_EQ(asked.lens.fy, expected.fy);
  }
  EXPECT_EQ(registration_.refinedLens.width, kWidth);
  EXPECT_EQ(registration_.refinedLens.height, kHeight);
  EXPECT_EQ(registration_.refinedLens.fx, expected.fx);
}

TEST_F(PanoramaBuildManagerTest, EveryFrameIsPriorToTheSolveWithThePoseItWasTakenAt) {
  CaptureRing(12);
  document_.candidates[4].pose.confidence = 0.0;
  WriteDocument();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  RunToEnd(build.value);
  ASSERT_EQ(registration_.refinedPriors.size(), 12u);
  for (size_t i = 0; i < 12; ++i) {
    EXPECT_EQ(registration_.refinedPriors[i].frame, document_.candidates[i].frame.id);
    EXPECT_EQ(registration_.refinedPriors[i].pose.confidence,
              document_.candidates[i].pose.confidence);
    EXPECT_EQ(AngleBetween(registration_.refinedPriors[i].pose.orientation,
                           document_.candidates[i].pose.orientation), 0.0);
  }
}

// A declined edge is not a failed build: the frame is still placed, by its other pairs or its
// prior. Every other refusal from a pair is.
TEST_F(PanoramaBuildManagerTest, APairThatDidNotRegisterIsLeftOut) {
  CaptureRing(12);
  const uint64_t a = document_.candidates[3].frame.id.value;
  const uint64_t b = document_.candidates[4].frame.id.value;
  registration_.pairFailures[{a, b}] = Fail(StatusCode::RegistrationFailed, "test", "no overlap");
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  EXPECT_EQ(RunToEnd(build.value).back().stage, BuildStage::Complete);
  EXPECT_EQ(registration_.refinedPairs.size(), 11u);
}

TEST_F(PanoramaBuildManagerTest, APairThatCouldNotBeAskedFailsTheBuild) {
  CaptureRing(12);
  const uint64_t a = document_.candidates[3].frame.id.value;
  const uint64_t b = document_.candidates[4].frame.id.value;
  registration_.pairFailures[{a, b}] = Fail(StatusCode::Internal, "test", "broken");
  const int64_t before = HeapUsed();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  const BuildProgress last = RunToEnd(build.value).back();
  EXPECT_EQ(last.stage, BuildStage::Failed);
  EXPECT_EQ(last.failure.code, StatusCode::Internal);
  EXPECT_EQ(registration_.refines, 0);
  EXPECT_EQ(HeapUsed(), before) << "a failed build kept its feature sets";
}

TEST_F(PanoramaBuildManagerTest, AFrameWithNoFeaturesIsPairedWithNothing) {
  CaptureRing(12);
  const FrameId blank = document_.candidates[5].frame.id;
  registration_.featureless.insert(blank.value);
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  EXPECT_EQ(RunToEnd(build.value).back().stage, BuildStage::Complete);
  for (const auto& asked : registration_.asked) {
    EXPECT_NE(asked.a, blank);
    EXPECT_NE(asked.b, blank);
  }
  EXPECT_EQ(registration_.asked.size(), 10u);
  EXPECT_EQ(registration_.refinedPriors.size(), 12u);
}

// ------------------------------------------------------------------ what the build gives back

TEST_F(PanoramaBuildManagerTest, FeatureSetsAreForgottenOnceTheLastPairIsEstimated) {
  CaptureRing(4);
  const int64_t before = HeapUsed();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  // Four extractions and the three pairs of an arc a quarter of the way round. Each set is still
  // held after the last extraction, and none after the last pair.
  for (int step = 0; step < 4; ++step) ASSERT_TRUE(manager_.Poll(build.value).ok());
  EXPECT_GT(HeapUsed(), before);
  for (int step = 0; step < 3; ++step) ASSERT_TRUE(manager_.Poll(build.value).ok());
  ASSERT_EQ(registration_.asked.size(), 3u);
  EXPECT_EQ(HeapUsed(), before);
}

// Extraction leaves a spilled frame resident, and a sphere's worth of those is the heap refusal
// cooling exists to avoid (ADR 0023).
TEST_F(PanoramaBuildManagerTest, AFrameGoesBackToTheTierItWasFoundIn) {
  CaptureRing(3);
  const FrameRef cold = document_.candidates[1].frame;
  ASSERT_TRUE(store_.Demote(cold, Residency::Spilled).ok());
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  for (int step = 0; step < 3; ++step) ASSERT_TRUE(manager_.Poll(build.value).ok());
  ASSERT_EQ(registration_.extracted.size(), 3u);
  auto residency = store_.ResidencyOf(cold);
  ASSERT_TRUE(residency.ok());
  EXPECT_EQ(residency.value, Residency::Spilled);
}

TEST_F(PanoramaBuildManagerTest, AFailedExtractionFailsTheBuildAndKeepsNothing) {
  CaptureRing(4);
  registration_.extractFailures[document_.candidates[2].frame.id.value] =
      Fail(StatusCode::FrameStoreExhausted, "test", "full");
  const int64_t before = HeapUsed();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  const BuildProgress last = RunToEnd(build.value).back();
  EXPECT_EQ(last.stage, BuildStage::Failed);
  EXPECT_EQ(last.failure.code, StatusCode::FrameStoreExhausted);
  EXPECT_EQ(HeapUsed(), before);
  EXPECT_EQ(manager_.Panorama(build.value).status.code, StatusCode::FailedPrecondition);

  // A failed build is finished: asked again it answers the same and reads nothing more.
  const size_t extracted = registration_.extracted.size();
  auto again = manager_.Poll(build.value);
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(again.value.stage, BuildStage::Failed);
  EXPECT_EQ(again.value.failure.code, StatusCode::FrameStoreExhausted);
  EXPECT_EQ(registration_.extracted.size(), extracted);

  // And it does not stand in the way of the next one.
  registration_.extractFailures.clear();
  auto next = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(next.ok()) << next.status.detail;
  EXPECT_EQ(RunToEnd(next.value).back().stage, BuildStage::Complete);
}

TEST_F(PanoramaBuildManagerTest, AFailedSolveFailsTheBuild) {
  CaptureRing(4);
  registration_.refineFailure = Fail(StatusCode::FailedPrecondition, "test", "no anchor");
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  const BuildProgress last = RunToEnd(build.value).back();
  EXPECT_EQ(last.stage, BuildStage::Failed);
  EXPECT_EQ(last.failure.code, StatusCode::FailedPrecondition);
  EXPECT_EQ(composition_.renders, 0);
}

// The compositor reads frames in the solution's order, and a frame the solve dropped is not one
// of them.
TEST_F(PanoramaBuildManagerTest, ThePreviewIsHandedTheSolutionsFramesInItsOrder) {
  CaptureRing(5);
  registration_.dropped.insert(document_.candidates[2].frame.id.value);
  registration_.reverse = true;
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  ASSERT_EQ(RunToEnd(build.value).back().stage, BuildStage::Complete);
  const GlobalSolution& solved = composition_.renderedSolution;
  ASSERT_EQ(composition_.renderedFrames.size(), 4u);
  for (size_t i = 0; i < solved.frames.size(); ++i) {
    EXPECT_EQ(composition_.renderedFrames[i].id, solved.frames[i]);
  }
}

TEST_F(PanoramaBuildManagerTest, APanoramaIsThePreviewAtTheSpecsWidthOrTwoThousand) {
  CaptureRing(3);
  BuildSpec small;
  small.outputWidth = 512;
  auto build = manager_.Start(kProject, small);
  ASSERT_TRUE(build.ok());
  EXPECT_EQ(manager_.Panorama(build.value).status.code, StatusCode::FailedPrecondition);
  ASSERT_EQ(RunToEnd(build.value).back().stage, BuildStage::Complete);
  EXPECT_EQ(composition_.renderedWidth, 512);
  auto panorama = manager_.Panorama(build.value);
  ASSERT_TRUE(panorama.ok()) << panorama.status.detail;
  EXPECT_EQ(panorama.value.build, build.value);
  EXPECT_EQ(panorama.value.projection, Projection::Equirectangular);
  EXPECT_EQ(panorama.value.width, 512);
  EXPECT_EQ(panorama.value.height, 256);
  EXPECT_TRUE(panorama.value.tiles.empty());
  EXPECT_TRUE(store_.ResidencyOf(panorama.value.preview).ok());

  ASSERT_TRUE(manager_.Cancel(build.value).ok());
  auto wide = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(wide.ok());
  RunToEnd(wide.value);
  EXPECT_EQ(composition_.renderedWidth, 2048);
}

TEST_F(PanoramaBuildManagerTest, APreviewThatCouldNotBeMadeIsGivenBack) {
  CaptureRing(3);
  composition_.failure = Fail(StatusCode::Internal, "test", "would not take it back");
  composition_.handBackPinned = true;
  const int64_t before = HeapUsed();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  const BuildProgress last = RunToEnd(build.value).back();
  EXPECT_EQ(last.stage, BuildStage::Failed);
  EXPECT_EQ(last.failure.code, StatusCode::Internal);
  EXPECT_EQ(HeapUsed(), before);
}

// A refusal can leave the frames it was handed pinned and faulted in, and they are the capture's:
// the build puts them back as it does after an extraction (`RenderPreview`), since a frame left
// pinned is one the next capture's `Clear` refuses to empty the store around.
TEST_F(PanoramaBuildManagerTest, ARefusedPreviewPutsTheCaptureBack) {
  CaptureRing(3);
  const FrameRef cold = document_.candidates[1].frame;
  ASSERT_TRUE(store_.Demote(cold, Residency::Spilled).ok());
  composition_.failure = Fail(StatusCode::Internal, "test", "would not release a frame");
  composition_.leaveInputsPinned = true;
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  ASSERT_EQ(RunToEnd(build.value).back().stage, BuildStage::Failed);
  for (const Candidate& candidate : document_.candidates) {
    auto residency = store_.ResidencyOf(candidate.frame);
    ASSERT_TRUE(residency.ok());
    EXPECT_NE(residency.value, Residency::HeapPinned) << "frame " << candidate.frame.id.value;
  }
  EXPECT_EQ(store_.ResidencyOf(cold).value, Residency::Spilled);
  EXPECT_TRUE(store_.Clear().ok()) << "a frame left pinned keeps the next capture from starting";
}

TEST_F(PanoramaBuildManagerTest, ACaptureFrameTheStoreWouldNotPutBackIsTriedAgainOnCancel) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  const FrameRef cold = document_.candidates[1].frame;
  ASSERT_TRUE(store_.Demote(cold, Residency::Spilled).ok());
  composition_.failure = Fail(StatusCode::Internal, "test", "would not release a frame");
  composition_.leaveInputsPinned = true;
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  // Three extractions, the arc's two pairs and the solve; the next step is the preview.
  for (int step = 0; step < 6; ++step) ASSERT_TRUE(manager.Poll(build.value).ok());
  reluctant.releaseRefusals = 3;
  auto failed = manager.Poll(build.value);
  ASSERT_EQ(failed.value.stage, BuildStage::Failed);
  // The preview's own refusal, with what could not be put right beside it for a person.
  EXPECT_EQ(failed.value.failure.code, StatusCode::Internal);
  EXPECT_NE(failed.value.failure.detail.find("will not release it this time"), std::string::npos);
  ASSERT_EQ(reluctant.releaseRefusals, 0);
  EXPECT_EQ(store_.ResidencyOf(cold).value, Residency::HeapPinned);
  // A retry the store refuses is answered, and kept for the next.
  const FrameRef first = document_.candidates[0].frame;
  reluctant.releaseRefusals = 1;
  EXPECT_EQ(manager.Cancel(build.value).code, StatusCode::Internal);
  EXPECT_EQ(store_.ResidencyOf(first).value, Residency::HeapPinned);
  EXPECT_TRUE(manager.Cancel(build.value).ok());
  EXPECT_EQ(store_.ResidencyOf(first).value, Residency::HeapEncoded);
  EXPECT_EQ(store_.ResidencyOf(cold).value, Residency::Spilled);
  EXPECT_TRUE(store_.Clear().ok());
}

// Asked before the call, because it is the only way to put right what a refusal leaves out of place
// — so a tier that cannot be read is a preview not drawn, rather than a frame nothing puts back.
TEST_F(PanoramaBuildManagerTest, ATierThatCannotBeReadRefusesThePreviewBeforeItIsDrawn) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  composition_.failure = Fail(StatusCode::Internal, "test", "would not release a frame");
  composition_.leaveInputsPinned = true;
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  for (int step = 0; step < 6; ++step) ASSERT_TRUE(manager.Poll(build.value).ok());
  reluctant.residencyRefusals = 1;
  auto failed = manager.Poll(build.value);
  ASSERT_EQ(failed.value.stage, BuildStage::Failed);
  EXPECT_EQ(failed.value.failure.detail, "cannot say where it is");
  EXPECT_EQ(composition_.renders, 0);
  EXPECT_TRUE(manager.Cancel(build.value).ok());
  EXPECT_TRUE(store_.Clear().ok());
}

// After the call, a frame whose tier cannot be read is one the build cannot say it put back, so it
// is kept and asked about again.
TEST_F(PanoramaBuildManagerTest, AFrameWhoseTierCannotBeReadAfterThePreviewIsTriedAgain) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  const FrameRef first = document_.candidates[0].frame;
  composition_.failure = Fail(StatusCode::Internal, "test", "would not release a frame");
  composition_.leaveInputsPinned = true;
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  for (int step = 0; step < 6; ++step) ASSERT_TRUE(manager.Poll(build.value).ok());
  // The three tiers asked before the call, then the first frame's after it.
  reluctant.residencyAnswersFirst = 3;
  reluctant.residencyRefusals = 1;
  auto failed = manager.Poll(build.value);
  ASSERT_EQ(failed.value.stage, BuildStage::Failed);
  ASSERT_EQ(reluctant.residencyRefusals, 0);
  EXPECT_NE(failed.value.failure.detail.find("cannot say where it is"), std::string::npos);
  EXPECT_EQ(store_.ResidencyOf(first).value, Residency::HeapPinned);
  EXPECT_TRUE(manager.Cancel(build.value).ok());
  EXPECT_EQ(store_.ResidencyOf(first).value, Residency::HeapEncoded);
  EXPECT_TRUE(store_.Clear().ok());
}

// A retry comes later, and by then a pin on the frame may be somebody else's — `Pin` promises its
// mapping until *their* release. The build releases the one pin it owes, once.
TEST_F(PanoramaBuildManagerTest, ARetryReleasesOnlyThePinTheBuildOwes) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  const FrameRef cold = document_.candidates[1].frame;
  ASSERT_TRUE(store_.Demote(cold, Residency::Spilled).ok());
  composition_.failure = Fail(StatusCode::Internal, "test", "would not release a frame");
  composition_.leaveInputsPinned = true;
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  for (int step = 0; step < 6; ++step) ASSERT_TRUE(manager.Poll(build.value).ok());
  reluctant.demoteRefusals = 1;
  ASSERT_EQ(manager.Poll(build.value).value.stage, BuildStage::Failed);
  ASSERT_EQ(reluctant.demoteRefusals, 0);
  ASSERT_EQ(store_.ResidencyOf(cold).value, Residency::HeapEncoded);
  ASSERT_TRUE(store_.Pin(cold).ok());
  EXPECT_TRUE(manager.Cancel(build.value).ok());
  EXPECT_EQ(store_.ResidencyOf(cold).value, Residency::HeapPinned);
  EXPECT_TRUE(store_.Release(cold).ok()) << "the build released a pin that was not its own";
}

// The build only ever faults a frame in, so putting one back only ever cools it. A frame something
// else has cooled since is where that component wants it, and warming it again would spend heap on
// a capture frame nobody is reading.
TEST_F(PanoramaBuildManagerTest, ARetryLeavesAFrameSomethingElseHasCooledSince) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  const FrameRef first = document_.candidates[0].frame;
  composition_.failure = Fail(StatusCode::Internal, "test", "refused with nothing out of place");
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  for (int step = 0; step < 6; ++step) ASSERT_TRUE(manager.Poll(build.value).ok());
  reluctant.residencyAnswersFirst = 3;
  reluctant.residencyRefusals = 1;
  ASSERT_EQ(manager.Poll(build.value).value.stage, BuildStage::Failed);
  ASSERT_EQ(reluctant.residencyRefusals, 0);
  // The capture cools the cell, as it does the moment a cell's burst is ranked (ADR 0023).
  ASSERT_TRUE(store_.Demote(first, Residency::Spilled).ok());
  const int64_t before = HeapUsed();
  EXPECT_TRUE(manager.Cancel(build.value).ok());
  EXPECT_EQ(store_.ResidencyOf(first).value, Residency::Spilled);
  EXPECT_EQ(HeapUsed(), before);
}

// Nothing in the tree holds a pin across calls, but `Pin`'s contract allows it — and a frame the
// build found pinned is one whose pins are all somebody else's.
TEST_F(PanoramaBuildManagerTest, AFrameSomebodyElseHoldsPinnedKeepsItsPin) {
  CaptureRing(3);
  const FrameRef held = document_.candidates[0].frame;
  ASSERT_TRUE(store_.Pin(held).ok());
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  ASSERT_EQ(RunToEnd(build.value).back().stage, BuildStage::Complete);
  EXPECT_EQ(store_.ResidencyOf(held).value, Residency::HeapPinned);
  EXPECT_TRUE(store_.Release(held).ok()) << "the build released a pin that was not its own";
}

// Only a store that says a frame is gone has given it back: one that cannot say where the panorama
// is has not, and forgetting the build over it would leave 8 MB in the heap that nothing names.
TEST_F(PanoramaBuildManagerTest, APanoramaWhoseTierCannotBeReadIsKeptForTheNextCancel) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  const int64_t before = HeapUsed();
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  ASSERT_EQ(RunToEnd(manager, build.value).back().stage, BuildStage::Complete);
  reluctant.residencyRefusals = 1;
  EXPECT_EQ(manager.Cancel(build.value).code, StatusCode::Internal);
  EXPECT_TRUE(manager.Cancel(build.value).ok());
  EXPECT_EQ(HeapUsed(), before);
}

// A frame found pinned has no colder tier to go back to, and a retry after its holder let go must
// not try to demote it into one — `HeapPinned` is not a tier a demotion can produce, and asking
// for it would refuse every `Cancel` and `Start` from then on.
TEST_F(PanoramaBuildManagerTest, AFrameFoundPinnedIsLeftWhereItIsAfterItsHolderLetsGo) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  const FrameRef held = document_.candidates[0].frame;
  ASSERT_TRUE(store_.Pin(held).ok());
  composition_.failure = Fail(StatusCode::Internal, "test", "refused with nothing out of place");
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  for (int step = 0; step < 6; ++step) ASSERT_TRUE(manager.Poll(build.value).ok());
  reluctant.residencyAnswersFirst = 3;
  reluctant.residencyRefusals = 1;
  ASSERT_EQ(manager.Poll(build.value).value.stage, BuildStage::Failed);
  ASSERT_EQ(reluctant.residencyRefusals, 0);
  ASSERT_TRUE(store_.Release(held).ok());
  EXPECT_TRUE(manager.Cancel(build.value).ok());
  EXPECT_TRUE(manager.Start(kProject, BuildSpec{}).ok());
}

// Pins are counted, so the build's release leaves a frame pinned while somebody else holds it too —
// and a demotion of a pinned frame is refused. The frame is left to its holder.
TEST_F(PanoramaBuildManagerTest, AFrameStillPinnedAfterTheBuildsReleaseIsLeftToItsHolder) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  const FrameRef cold = document_.candidates[1].frame;
  ASSERT_TRUE(store_.Demote(cold, Residency::Spilled).ok());
  composition_.failure = Fail(StatusCode::Internal, "test", "would not release a frame");
  composition_.leaveInputsPinned = true;
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  for (int step = 0; step < 6; ++step) ASSERT_TRUE(manager.Poll(build.value).ok());
  reluctant.releaseRefusals = 3;
  ASSERT_EQ(manager.Poll(build.value).value.stage, BuildStage::Failed);
  ASSERT_TRUE(store_.Pin(cold).ok());
  const Status cancelled = manager.Cancel(build.value);
  EXPECT_TRUE(cancelled.ok()) << cancelled.detail;
  EXPECT_EQ(store_.ResidencyOf(cold).value, Residency::HeapPinned);
  EXPECT_TRUE(store_.Release(cold).ok()) << "the build released a pin that was not its own";
}

// A tier that cannot be read after the build's release is one it cannot say it put back, so the
// frame is kept — having paid the pin, it owes nothing more.
TEST_F(PanoramaBuildManagerTest, AFrameWhoseTierCannotBeReadAfterItsReleaseIsTriedAgain) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  const FrameRef cold = document_.candidates[0].frame;
  ASSERT_TRUE(store_.Demote(cold, Residency::Spilled).ok());
  composition_.failure = Fail(StatusCode::Internal, "test", "would not release a frame");
  composition_.leaveInputsPinned = true;
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  for (int step = 0; step < 6; ++step) ASSERT_TRUE(manager.Poll(build.value).ok());
  // The three tiers asked before the call and the first frame's after it; then its re-read.
  reluctant.residencyAnswersFirst = 4;
  reluctant.residencyRefusals = 1;
  ASSERT_EQ(manager.Poll(build.value).value.stage, BuildStage::Failed);
  ASSERT_EQ(reluctant.residencyRefusals, 0);
  // Released and not yet put back: the refusal landed on the read after the release, not before it.
  ASSERT_EQ(store_.ResidencyOf(cold).value, Residency::HeapEncoded);
  EXPECT_TRUE(manager.Cancel(build.value).ok());
  EXPECT_EQ(store_.ResidencyOf(cold).value, Residency::Spilled);
}

TEST_F(PanoramaBuildManagerTest, APinPaidBeforeARefusedReadIsNotPaidAgain) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  const FrameRef cold = document_.candidates[0].frame;
  ASSERT_TRUE(store_.Demote(cold, Residency::Spilled).ok());
  composition_.failure = Fail(StatusCode::Internal, "test", "would not release a frame");
  composition_.leaveInputsPinned = true;
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  for (int step = 0; step < 6; ++step) ASSERT_TRUE(manager.Poll(build.value).ok());
  reluctant.residencyAnswersFirst = 4;
  reluctant.residencyRefusals = 1;
  ASSERT_EQ(manager.Poll(build.value).value.stage, BuildStage::Failed);
  ASSERT_EQ(reluctant.residencyRefusals, 0);
  // Released and not yet put back: the refusal landed on the read after the release, not before it.
  ASSERT_EQ(store_.ResidencyOf(cold).value, Residency::HeapEncoded);
  ASSERT_TRUE(store_.Pin(cold).ok());
  EXPECT_TRUE(manager.Cancel(build.value).ok());
  EXPECT_TRUE(store_.Release(cold).ok()) << "the build released a pin that was not its own";
}

// An engine that could not release leaves one pin per call that borrowed the frame, and a feature
// frame is borrowed by every pair it is in — so on a ring, two.
TEST_F(PanoramaBuildManagerTest, AFeatureFrameEveryPairLeftPinnedIsStillGivenBack) {
  CaptureRing(12);
  registration_.leaveFeaturesPinned = true;
  const int64_t before = HeapUsed();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  const BuildProgress last = RunToEnd(build.value).back();
  EXPECT_EQ(last.stage, BuildStage::Complete) << last.failure.detail;
  EXPECT_TRUE(manager_.Cancel(build.value).ok());
  EXPECT_EQ(HeapUsed(), before);
}

// A feature set's frames are the build's alone, so a pin on one is an engine's that could not
// release it — the real engine's `BorrowedFrame` discards a refused `Release` — and nothing but
// the build can name the frame to release it.
TEST_F(PanoramaBuildManagerTest, AFeatureFrameAnEngineLeftPinnedIsStillGivenBack) {
  CaptureRing(3);
  registration_.leaveFeaturesPinned = true;
  const int64_t before = HeapUsed();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  const BuildProgress last = RunToEnd(build.value).back();
  EXPECT_EQ(last.stage, BuildStage::Complete) << last.failure.detail;
  EXPECT_TRUE(manager_.Cancel(build.value).ok());
  EXPECT_EQ(HeapUsed(), before);
  EXPECT_TRUE(store_.Clear().ok());
}

TEST_F(PanoramaBuildManagerTest, CancelGivesBackEverythingAndForgetsTheBuild) {
  CaptureRing(6);
  const int64_t before = HeapUsed();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  for (int step = 0; step < 4; ++step) ASSERT_TRUE(manager_.Poll(build.value).ok());
  ASSERT_GT(HeapUsed(), before);
  ASSERT_TRUE(manager_.Cancel(build.value).ok());
  EXPECT_EQ(HeapUsed(), before);
  EXPECT_EQ(manager_.Poll(build.value).status.code, StatusCode::NotFound);
  EXPECT_EQ(manager_.Cancel(build.value).code, StatusCode::NotFound);
  EXPECT_TRUE(manager_.Start(kProject, BuildSpec{}).ok());
}

TEST_F(PanoramaBuildManagerTest, CancellingACompleteBuildGivesBackItsPanorama) {
  CaptureRing(3);
  const int64_t before = HeapUsed();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  ASSERT_EQ(RunToEnd(build.value).back().stage, BuildStage::Complete);
  ASSERT_GT(HeapUsed(), before);
  ASSERT_TRUE(manager_.Cancel(build.value).ok());
  EXPECT_EQ(HeapUsed(), before);
}

TEST_F(PanoramaBuildManagerTest, StartingAgainReleasesTheFinishedBuild) {
  CaptureRing(3);
  const int64_t before = HeapUsed();
  auto first = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(first.ok());
  ASSERT_EQ(RunToEnd(first.value).back().stage, BuildStage::Complete);
  auto second = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(second.ok()) << second.status.detail;
  EXPECT_NE(second.value, first.value);
  EXPECT_EQ(HeapUsed(), before);
  EXPECT_EQ(manager_.Panorama(first.value).status.code, StatusCode::NotFound);
}

TEST_F(PanoramaBuildManagerTest, GhostsAndInvalidateAreNotBuiltYet) {
  CaptureRing(3);
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  const NodeId dirty[] = {NodeId{1}};
  EXPECT_EQ(manager_.Ghosts(build.value).status.code, StatusCode::Unsupported);
  EXPECT_EQ(manager_.Invalidate(build.value, dirty).code, StatusCode::Unsupported);
  EXPECT_EQ(manager_.Ghosts(BuildId{99}).status.code, StatusCode::NotFound);
  EXPECT_EQ(manager_.Invalidate(BuildId{99}, dirty).code, StatusCode::NotFound);
}

// The capture manager empties the store when a new capture begins (ADR 0034), and a build holds no
// pin between two `Poll`s to stop it. What the build held is gone then, not refused: a `Forget`
// answering `NotFound` has nothing left to retry, and a build that kept retrying it could never be
// cancelled or replaced.
TEST_F(PanoramaBuildManagerTest, AStoreEmptiedUnderAFinishedBuildDoesNotStrandIt) {
  CaptureRing(3);
  auto first = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(first.ok());
  ASSERT_EQ(RunToEnd(first.value).back().stage, BuildStage::Complete);
  ASSERT_TRUE(store_.Clear().ok());
  // And its panorama is not handed out as though the store still held it.
  EXPECT_EQ(manager_.Panorama(first.value).status.code, StatusCode::FailedPrecondition);

  document_.candidates.clear();
  CaptureRing(3);
  auto second = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(second.ok()) << second.status.detail;
  EXPECT_EQ(RunToEnd(second.value).back().stage, BuildStage::Complete);
}

TEST_F(PanoramaBuildManagerTest, AStoreEmptiedUnderARunningBuildDoesNotStrandIt) {
  CaptureRing(4);
  auto first = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(first.ok());
  for (int step = 0; step < 2; ++step) ASSERT_TRUE(manager_.Poll(first.value).ok());
  ASSERT_TRUE(store_.Clear().ok());
  auto failed = manager_.Poll(first.value);
  ASSERT_TRUE(failed.ok());
  EXPECT_EQ(failed.value.stage, BuildStage::Failed);
  EXPECT_TRUE(manager_.Cancel(first.value).ok());

  document_.candidates.clear();
  CaptureRing(3);
  auto second = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(second.ok()) << second.status.detail;
  EXPECT_EQ(RunToEnd(second.value).back().stage, BuildStage::Complete);
}

// Only a store that says the panorama is gone has said so; one that cannot answer has not, and
// "build again" would spend a whole build on a panorama that is still there.
TEST_F(PanoramaBuildManagerTest, APanoramaWhoseTierCannotBeReadIsNotAGonePanorama) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  ASSERT_EQ(RunToEnd(manager, build.value).back().stage, BuildStage::Complete);
  reluctant.residencyRefusals = 1;
  EXPECT_EQ(manager.Panorama(build.value).status.code, StatusCode::Internal);
  EXPECT_TRUE(manager.Panorama(build.value).ok());
}

uint8_t Patterned(size_t i) { return static_cast<uint8_t>(i * 7 + (i >> 8) * 13 + 3); }

// What the page draws: the preview's own pixels, row by row, copied out of the store — the page has
// no store to resolve a handle against (ADR 0071).
TEST_F(PanoramaBuildManagerTest, ThePageIsHandedThePanoramasPixels) {
  CaptureRing(3);
  BuildSpec small;
  small.outputWidth = 64;
  auto build = manager_.Start(kProject, small);
  ASSERT_TRUE(build.ok());
  ASSERT_EQ(RunToEnd(build.value).back().stage, BuildStage::Complete);
  auto panorama = manager_.Panorama(build.value);
  ASSERT_TRUE(panorama.ok());
  const FrameRef& frame = panorama.value.preview;
  {
    auto bytes = store_.Pin(frame);
    ASSERT_TRUE(bytes.ok());
    // A row is 256 bytes, so the pattern changes with the row as well as along it: one that repeated
    // every 256 bytes would be satisfied by every row copied from the first.
    for (size_t i = 0; i < bytes.value.size(); ++i) bytes.value[i] = Patterned(i);
    ASSERT_TRUE(store_.Release(frame).ok());
  }

  auto preview = manager_.PanoramaPreview(build.value);
  ASSERT_TRUE(preview.ok()) << preview.status.detail;
  EXPECT_EQ(preview.value.frame, frame.id);
  EXPECT_EQ(preview.value.width, 64);
  EXPECT_EQ(preview.value.height, 32);
  EXPECT_EQ(preview.value.format, PixelFormat::RGBA8);
  ASSERT_EQ(preview.value.pixels.size(), size_t{64} * 32 * 4);
  for (size_t i = 0; i < preview.value.pixels.size(); ++i) {
    ASSERT_EQ(preview.value.pixels[i], Patterned(i)) << "byte " << i;
  }
  // And nothing is left pinned: the build still holds the panorama, in the heap, for the next ask.
  auto residency = store_.ResidencyOf(frame);
  ASSERT_TRUE(residency.ok());
  EXPECT_NE(residency.value, Residency::HeapPinned);
  EXPECT_TRUE(manager_.PanoramaPreview(build.value).ok());
}

// The same refusals `Panorama` gives, for the same reasons.
TEST_F(PanoramaBuildManagerTest, OnlyACompleteBuildsPixelsAreHanded) {
  EXPECT_EQ(manager_.PanoramaPreview(BuildId{7}).status.code, StatusCode::NotFound);
  CaptureRing(3);
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  EXPECT_EQ(manager_.PanoramaPreview(build.value).status.code, StatusCode::FailedPrecondition);
  ASSERT_EQ(RunToEnd(build.value).back().stage, BuildStage::Complete);
  ASSERT_TRUE(manager_.PanoramaPreview(build.value).ok());
  ASSERT_TRUE(store_.Clear().ok());
  EXPECT_EQ(manager_.PanoramaPreview(build.value).status.code, StatusCode::FailedPrecondition);
}

// A store that will not fault the panorama in, or let go of it, is answered with its own status. A
// pin a refused release leaves is the build's to give back, and `Cancel` does.
TEST_F(PanoramaBuildManagerTest, APanoramaTheStoreWillNotReadOrReleaseIsAnsweredWithItsStatus) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  const int64_t captured = HeapUsed();
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  ASSERT_EQ(RunToEnd(manager, build.value).back().stage, BuildStage::Complete);

  reluctant.pinRefusals = 1;
  EXPECT_EQ(manager.PanoramaPreview(build.value).status.code, StatusCode::Internal);
  reluctant.releaseRefusals = 1;
  EXPECT_EQ(manager.PanoramaPreview(build.value).status.code, StatusCode::Internal);
  EXPECT_TRUE(manager.PanoramaPreview(build.value).ok());

  ASSERT_TRUE(manager.Cancel(build.value).ok());
  EXPECT_EQ(HeapUsed(), captured);
}

// `RenderPreview` hands its answer back pinned when the store would not release it, and the build
// then releases and forgets it — on every retry, not once: a pin nothing retries is 8 MB that
// `Cancel` can never give back, however willing the store becomes.
TEST_F(PanoramaBuildManagerTest, AHandedBackPreviewIsReleasedWhenTheStoreWillAgain) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  composition_.failure = Fail(StatusCode::Internal, "test", "would not take it back");
  composition_.handBackPinned = true;
  const int64_t before = HeapUsed();
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  // Three extractions, the arc's two pairs and the solve; the next step is the preview.
  for (int step = 0; step < 6; ++step) ASSERT_TRUE(manager.Poll(build.value).ok());
  reluctant.releaseRefusals = 1;
  auto composed = manager.Poll(build.value);
  ASSERT_TRUE(composed.ok());
  ASSERT_EQ(composed.value.stage, BuildStage::Failed);
  ASSERT_EQ(reluctant.releaseRefusals, 0) << "the build never tried to release the preview";
  ASSERT_GT(HeapUsed(), before);
  EXPECT_TRUE(manager.Cancel(build.value).ok());
  EXPECT_EQ(HeapUsed(), before);
}

// A refusal from an engine that had already read the frame leaves it faulted in, and the want of
// room that refusal usually means is what a resident capture frame makes worse.
TEST_F(PanoramaBuildManagerTest, AFrameGoesBackToItsTierWhenItsExtractionIsRefused) {
  CaptureRing(3);
  const FrameRef cold = document_.candidates[1].frame;
  ASSERT_TRUE(store_.Demote(cold, Residency::Spilled).ok());
  registration_.extractFailures[cold.id.value] =
      Fail(StatusCode::FrameStoreExhausted, "test", "no room for the answer");
  registration_.failAfterReading = true;
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  const BuildProgress last = RunToEnd(build.value).back();
  ASSERT_EQ(last.stage, BuildStage::Failed);
  EXPECT_EQ(last.failure.code, StatusCode::FrameStoreExhausted);
  auto residency = store_.ResidencyOf(cold);
  ASSERT_TRUE(residency.ok());
  EXPECT_EQ(residency.value, Residency::Spilled);
}

TEST_F(PanoramaBuildManagerTest, AFrameThatCannotGoBackToItsTierFailsTheBuild) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  ASSERT_TRUE(store_.Demote(document_.candidates[0].frame, Residency::Spilled).ok());
  reluctant.demoteRefusals = 1;
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  auto polled = manager.Poll(build.value);
  ASSERT_TRUE(polled.ok());
  EXPECT_EQ(polled.value.stage, BuildStage::Failed);
  EXPECT_EQ(polled.value.failure.code, StatusCode::StorageQuotaExceeded);
  // And it is tried again, rather than left in the heap for good.
  EXPECT_EQ(store_.ResidencyOf(document_.candidates[0].frame).value, Residency::HeapEncoded);
  EXPECT_TRUE(manager.Cancel(build.value).ok());
  EXPECT_EQ(store_.ResidencyOf(document_.candidates[0].frame).value, Residency::Spilled);
}

// A new capture empties the store, and a frame it no longer holds has nowhere to be put back to.
TEST_F(PanoramaBuildManagerTest, AFrameLeftOutOfItsTierInAStoreSinceEmptiedDoesNotStrandTheBuild) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  ASSERT_TRUE(store_.Demote(document_.candidates[0].frame, Residency::Spilled).ok());
  reluctant.demoteRefusals = 1;
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  ASSERT_EQ(manager.Poll(build.value).value.stage, BuildStage::Failed);
  ASSERT_TRUE(store_.Clear().ok());
  EXPECT_TRUE(manager.Cancel(build.value).ok());
}

// With nothing to pair, the features are given back before the solve rather than held to the end.
TEST_F(PanoramaBuildManagerTest, ABuildWithNoPairsGivesItsFeaturesBackBeforeTheSolve) {
  AddCandidate(NodeId{1}, FromAzimuthElevation(0.0, 0.0));
  AddCandidate(NodeId{2}, FromAzimuthElevation(180.0, 0.0));
  WriteDocument();
  const int64_t before = HeapUsed();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  for (int step = 0; step < 3; ++step) ASSERT_TRUE(manager_.Poll(build.value).ok());
  ASSERT_EQ(registration_.refines, 1);
  EXPECT_TRUE(registration_.asked.empty());
  EXPECT_EQ(HeapUsed(), before);
}

TEST_F(PanoramaBuildManagerTest, ASolveThatPlacesAFrameTheBuildNeverReadFailsIt) {
  CaptureRing(3);
  registration_.placeAStranger = true;
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  const BuildProgress last = RunToEnd(build.value).back();
  EXPECT_EQ(last.stage, BuildStage::Failed);
  EXPECT_EQ(last.failure.code, StatusCode::Internal);
  EXPECT_EQ(composition_.renders, 0);
}

TEST_F(PanoramaBuildManagerTest, APreviewRefusedWithNothingHandedBackLeavesNothingHeld) {
  CaptureRing(3);
  composition_.failure = Fail(StatusCode::Unsupported, "test", "not a format it draws");
  const int64_t before = HeapUsed();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  EXPECT_EQ(RunToEnd(build.value).back().stage, BuildStage::Failed);
  EXPECT_EQ(HeapUsed(), before);
  EXPECT_TRUE(manager_.Cancel(build.value).ok());
}

// A `Forget` the store refuses keeps charging the bytes, so the build keeps the handle and the next
// `Cancel` asks again — and only then is the build gone.
TEST_F(PanoramaBuildManagerTest, ACancelTheStoreRefusesIsTriedAgain) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(4);
  const int64_t before = HeapUsed();
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  for (int step = 0; step < 3; ++step) ASSERT_TRUE(manager.Poll(build.value).ok());
  reluctant.forgetRefusals = 1;
  EXPECT_EQ(manager.Cancel(build.value).code, StatusCode::Internal);
  EXPECT_GT(HeapUsed(), before);
  auto still = manager.Poll(build.value);
  ASSERT_TRUE(still.ok()) << "a build whose cancel was refused is still there to cancel";
  EXPECT_EQ(still.value.stage, BuildStage::Failed);
  EXPECT_EQ(still.value.failure.code, StatusCode::Cancelled);
  EXPECT_TRUE(manager.Cancel(build.value).ok());
  EXPECT_EQ(HeapUsed(), before);
  EXPECT_EQ(manager.Poll(build.value).status.code, StatusCode::NotFound);
}

TEST_F(PanoramaBuildManagerTest, ACompleteBuildWhoseCancelIsRefusedStaysComplete) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  auto build = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  ASSERT_EQ(RunToEnd(manager, build.value).back().stage, BuildStage::Complete);
  reluctant.forgetRefusals = 1;
  EXPECT_EQ(manager.Cancel(build.value).code, StatusCode::Internal);
  EXPECT_EQ(manager.Poll(build.value).value.stage, BuildStage::Complete);
  EXPECT_TRUE(manager.Panorama(build.value).ok()) << "a panorama still held is still answered";
  EXPECT_TRUE(manager.Cancel(build.value).ok());
}

TEST_F(PanoramaBuildManagerTest, AFinishedBuildTheStoreWillNotReleaseStandsUntilItWill) {
  ReluctantFrameStore reluctant{store_};
  PanoramaBuildManager manager{registration_, composition_, reluctant, projects_};
  CaptureRing(3);
  const int64_t before = HeapUsed();
  auto first = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(first.ok());
  ASSERT_EQ(RunToEnd(manager, first.value).back().stage, BuildStage::Complete);
  reluctant.forgetRefusals = 1;
  EXPECT_EQ(manager.Start(kProject, BuildSpec{}).status.code, StatusCode::Internal);
  EXPECT_TRUE(manager.Panorama(first.value).ok()) << "the finished build did not stand";
  auto second = manager.Start(kProject, BuildSpec{});
  ASSERT_TRUE(second.ok()) << second.status.detail;
  EXPECT_EQ(HeapUsed(), before);
}

// `Refine` ignores an unaccepted pair by contract (ADR 0056), so the build hands it over rather
// than deciding for it.
TEST_F(PanoramaBuildManagerTest, AnUnacceptedPairIsHandedToTheSolve) {
  CaptureRing(12);
  const uint64_t a = document_.candidates[3].frame.id.value;
  const uint64_t b = document_.candidates[4].frame.id.value;
  registration_.unaccepted.insert({a, b});
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  RunToEnd(build.value);
  ASSERT_EQ(registration_.refinedPairs.size(), 12u);
  EXPECT_EQ(std::count_if(registration_.refinedPairs.begin(), registration_.refinedPairs.end(),
                          [](const PairwiseResult& pair) { return !pair.accepted; }),
            1);
}

// By the direction each frame looked, not by how far apart the whole orientations are: a frame
// held on its side looks where its neighbour looks.
TEST_F(PanoramaBuildManagerTest, AFrameRolledAgainstItsNeighbourStillPairsWithIt) {
  AddCandidate(NodeId{1}, FromAzimuthElevation(0.0, 0.0));
  AddCandidate(NodeId{2}, Multiply(FromAzimuthElevation(10.0, 0.0),
                                   FromAxisAngle(Vec3{0, 0, 1}, std::numbers::pi / 2)));
  WriteDocument();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok());
  RunToEnd(build.value);
  EXPECT_EQ(registration_.asked.size(), 1u);
}

// The narrower field of view decides the reach, whichever way the frame is held: fifty-five degrees
// apart is inside a 66-degree width and outside a 50-degree one.
TEST_F(PanoramaBuildManagerTest, TheNarrowerFieldOfViewDecidesWhichCellsPair) {
  AddCandidate(NodeId{1}, FromAzimuthElevation(0.0, 0.0));
  AddCandidate(NodeId{2}, FromAzimuthElevation(55.0, 0.0));
  WriteDocument();
  for (const bool portrait : {false, true}) {
    document_.spec.horizontalFovDeg = portrait ? kVerticalFovDeg : kHorizontalFovDeg;
    document_.spec.verticalFovDeg = portrait ? kHorizontalFovDeg : kVerticalFovDeg;
    ASSERT_TRUE(projects_.WriteDocument(kProject, kSessionDocumentKey,
                                        EncodeSessionDocument(document_)).ok());
    registration_.asked.clear();
    auto build = manager_.Start(kProject, BuildSpec{});
    ASSERT_TRUE(build.ok()) << build.status.detail;
    RunToEnd(build.value);
    EXPECT_TRUE(registration_.asked.empty()) << (portrait ? "portrait" : "landscape");
  }
}

// At confidence zero the orientation is not a measurement, and the direction a degenerate one
// normalises to is straight ahead — which would pair it with whatever looks that way.
TEST_F(PanoramaBuildManagerTest, AFrameWhosePoseWasNotMeasuredIsPairedWithNothing) {
  CaptureRing(12);
  // One first in the ring and one in the middle, so each is the second of a pair as well as the
  // first.
  for (const size_t unmeasured : {size_t{0}, size_t{6}}) {
    document_.candidates[unmeasured].pose.confidence = 0.0;
    document_.candidates[unmeasured].pose.orientation = Quat{0, 0, 0, 0};
  }
  WriteDocument();
  auto build = manager_.Start(kProject, BuildSpec{});
  ASSERT_TRUE(build.ok()) << build.status.detail;
  RunToEnd(build.value);
  for (const auto& asked : registration_.asked) {
    for (const size_t unmeasured : {size_t{0}, size_t{6}}) {
      EXPECT_NE(asked.a, document_.candidates[unmeasured].frame.id);
      EXPECT_NE(asked.b, document_.candidates[unmeasured].frame.id);
    }
  }
  EXPECT_EQ(registration_.asked.size(), 8u);
}

TEST_F(PanoramaBuildManagerTest, ABuildNobodyStartedIsNotFound) {
  EXPECT_EQ(manager_.Poll(BuildId{1}).status.code, StatusCode::NotFound);
  EXPECT_EQ(manager_.Panorama(BuildId{1}).status.code, StatusCode::NotFound);
  EXPECT_EQ(manager_.Cancel(BuildId{1}).code, StatusCode::NotFound);
}

}  // namespace
}  // namespace sphanorama
