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
    if (auto found = extractFailures.find(frame.id.value); found != extractFailures.end()) {
      return found->second;
    }
    auto pinned = store_.Pin(frame);
    if (!pinned.ok()) return pinned.status;
    if (auto released = store_.Release(frame); !released.ok()) return released;
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
    pair.accepted = true;
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
  std::set<uint64_t> featureless;
  std::map<std::pair<uint64_t, uint64_t>, Status> pairFailures;
  Status refineFailure;
  std::set<uint64_t> dropped;
  bool reverse = false;

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

 private:
  IFrameStoreAccess& store_;
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
  std::vector<BuildProgress> RunToEnd(BuildId build, int limit = 1000) {
    std::vector<BuildProgress> seen;
    for (int i = 0; i < limit; ++i) {
      auto progress = manager_.Poll(build);
      EXPECT_TRUE(progress.ok()) << progress.status.detail;
      if (!progress.ok()) break;
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
  AddCandidate(NodeId{1}, FromAzimuthElevation(0.0, 0.0));
  AddCandidate(NodeId{2}, FromAzimuthElevation(30.0, 0.0), kWidth * 2, kHeight * 2);
  WriteDocument();
  EXPECT_EQ(manager_.Start(kProject, BuildSpec{}).status.code, StatusCode::FailedPrecondition);
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

// The review screen shows the pick; a build that quietly used the ranking's instead would make a
// panorama from a frame nobody was shown.
TEST_F(PanoramaBuildManagerTest, APickNamingACandidateTheCellDoesNotHoldIsRefused) {
  AddCandidate(NodeId{1}, FromAzimuthElevation(0.0, 0.0));
  const Candidate elsewhere = AddCandidate(NodeId{2}, FromAzimuthElevation(30.0, 0.0));
  WriteDocument();
  ASSERT_TRUE(projects_.WriteDocument(kProject, SelectionDocumentKey(NodeId{1}),
                                      std::to_string(elsewhere.id.value)).ok());
  EXPECT_EQ(manager_.Start(kProject, BuildSpec{}).status.code, StatusCode::FailedPrecondition);
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
  EXPECT_EQ(stages.back(), BuildStage::Complete);
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

TEST_F(PanoramaBuildManagerTest, ABuildNobodyStartedIsNotFound) {
  EXPECT_EQ(manager_.Poll(BuildId{1}).status.code, StatusCode::NotFound);
  EXPECT_EQ(manager_.Panorama(BuildId{1}).status.code, StatusCode::NotFound);
  EXPECT_EQ(manager_.Cancel(BuildId{1}).code, StatusCode::NotFound);
}

}  // namespace
}  // namespace sphanorama
