// `IRegistrationEngine::Refine` over pairs and priors built by hand, so every answer is known.
//
// Apart from the engine's other tests because it needs no pixels: the pairs are the rotations an
// exact `EstimatePairwise` would have answered with, and the registered frames' own accuracy is
// `registration_accuracy_test.cpp`'s job.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>
#include <vector>

#include "engines/registration_engine/feature_registration_engine.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "support/rotation_scoring.h"
#include "support/same_rotation.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

using test::kSameRotationDeg;

constexpr double kDegPerRad = 180.0 / std::numbers::pi;
constexpr int kFrames = 12;

Quat AboutY(double degrees) { return FromAxisAngle(Vec3{0, 1, 0}, degrees / kDegPerRad); }

double SeparationDeg(const Quat& a, const Quat& b) { return AngleBetween(a, b) * kDegPerRad; }

FrameId Frame(int i) { return FrameId{static_cast<uint64_t>(100 + i)}; }

// A ring turning about `y`, frame `i` at `30 i` degrees, the size the accuracy dataset renders.
std::vector<Quat> Ring(int frames = kFrames) {
  std::vector<Quat> truth;
  for (int i = 0; i < frames; ++i) truth.push_back(AboutY(360.0 * i / frames));
  return truth;
}

// A usable lens with every field set to something no default would produce, so "passed through"
// is checked on all of them.
Intrinsics Lens() {
  Intrinsics lens;
  lens.fx = 512;
  lens.fy = 510;
  lens.cx = 320;
  lens.cy = 240;
  lens.k1 = -0.1;
  lens.k2 = 0.02;
  lens.k3 = -0.003;
  lens.p1 = 0.0004;
  lens.p2 = -0.0005;
  lens.width = 640;
  lens.height = 480;
  lens.rollingShutterLineTimeNs = 15000;
  lens.estimated = true;
  return lens;
}

// What `EstimatePairwise(a, b)` answers for an exact pair: `conjugate(q[b]) * q[a]`, spelled here
// rather than borrowed from the solver so a convention inverted in both would still be caught.
// The correspondences are not a multiple of the inliers, so weighing by one cannot pass for the
// other.
PairwiseResult Pair(int a, int b, const std::vector<Quat>& truth, int32_t inliers = 100,
                    int32_t correspondences = 180) {
  PairwiseResult pair;
  pair.a = Frame(a);
  pair.b = Frame(b);
  pair.relativeRotation = Normalize(Multiply(Conjugate(truth[static_cast<size_t>(b)]),
                                             truth[static_cast<size_t>(a)]));
  pair.inliers = inliers;
  pair.correspondences = correspondences;
  pair.accepted = true;
  return pair;
}

// Every consecutive pair and the one that closes the ring.
std::vector<PairwiseResult> RingPairs(const std::vector<Quat>& truth) {
  std::vector<PairwiseResult> pairs;
  const int n = static_cast<int>(truth.size());
  for (int i = 0; i < n; ++i) pairs.push_back(Pair(i, (i + 1) % n, truth));
  return pairs;
}

// A prior a working capture would hold: an anchored reading, which is what `ArmBurst` requires
// before it fires (ADR 0044).
FramePrior Prior(int i, const Quat& orientation) {
  FramePrior prior;
  prior.frame = Frame(i);
  prior.pose.orientation = orientation;
  prior.pose.confidence = 1.0;
  return prior;
}

// No prior, spelled the one way the contract spells it: a pose nobody set, whose confidence is zero.
FramePrior NoPrior(int i) {
  FramePrior prior;
  prior.frame = Frame(i);
  return prior;
}

// Priors each three degrees out about an axis of their own, which is how far a fused phone
// orientation is out when it is working.
std::vector<FramePrior> PriorsOut(const std::vector<Quat>& truth, double degrees = 3.0) {
  std::vector<FramePrior> priors;
  for (size_t i = 0; i < truth.size(); ++i) {
    const double at = static_cast<double>(i);
    const Vec3 axis{std::sin(at), std::cos(at), std::sin(2.0 * at)};
    priors.push_back(Prior(static_cast<int>(i),
                           Normalize(Multiply(truth[i], FromAxisAngle(axis, degrees / kDegPerRad)))));
  }
  return priors;
}

std::vector<Quat> Orientations(const std::vector<FramePrior>& priors) {
  std::vector<Quat> out;
  for (const FramePrior& prior : priors) out.push_back(prior.pose.orientation);
  return out;
}

class Refine : public ::testing::Test {
 protected:
  MemoryFrameStoreAccess store_{1 << 20};
  FeatureRegistrationEngine engine_{store_, FeatureDetector::Orb};
};

/**
 * Exact pairs and priors degrees out give back the truth, facing where the priors agree.
 *
 * The shape is the pairs' and the gauge is the priors': compared gauge-free the answer is the
 * truth, and the gauge that was removed is the one the priors agree on best. Both halves, because
 * the first alone is satisfied by a solve that ignores the priors and the second by one that
 * ignores the pairs.
 *
 * The priors are given in reverse, so the order the answer comes back in is theirs and not the
 * frames' own.
 */
TEST_F(Refine, ExactPairsGiveTheTruthFacingWhereThePriorsAgree) {
  const std::vector<Quat> truth = Ring();
  std::vector<FramePrior> priors = PriorsOut(truth);
  std::reverse(priors.begin(), priors.end());
  const Intrinsics lens = Lens();

  const Result<GlobalSolution> solved = engine_.Refine(RingPairs(truth), priors, lens);
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  const GlobalSolution& solution = solved.value;

  ASSERT_EQ(solution.frames.size(), truth.size());
  ASSERT_EQ(solution.rotations.size(), truth.size());
  std::vector<Quat> truthInOrder;
  for (size_t i = 0; i < priors.size(); ++i) {
    EXPECT_EQ(solution.frames[i], priors[i].frame) << "not in the priors' order";
    truthInOrder.push_back(truth[priors[i].frame.value - 100]);
  }
  EXPECT_TRUE(solution.converged);
  EXPECT_EQ(solution.edgesUsed, kFrames);
  EXPECT_EQ(solution.priorsUsed, kFrames);
  EXPECT_TRUE(solution.droppedFrames.empty());
  EXPECT_TRUE(solution.priorOnlyFrames.empty());
  EXPECT_TRUE(solution.ambiguousFrames.empty());

  const test::RotationScore shape = test::ScoreRotations(solution.rotations, truthInOrder);
  ASSERT_TRUE(shape.valid);
  EXPECT_LT(shape.maxDeg, 0.01) << "the pairs are exact, so only the priors' light pull remains";
  EXPECT_LT(solution.maxEdgeErrorDeg, 0.01);

  const test::GaugeAlignment agreed = test::BestGaugeAlignment(Orientations(priors), truthInOrder);
  ASSERT_TRUE(agreed.valid);
  EXPECT_NEAR(SeparationDeg(shape.alignment, agreed.rotation), 0.0, 1e-3);

  // Passed through, every field of it: nothing refines a lens yet.
  EXPECT_EQ(solution.intrinsics.fx, lens.fx);
  EXPECT_EQ(solution.intrinsics.fy, lens.fy);
  EXPECT_EQ(solution.intrinsics.cx, lens.cx);
  EXPECT_EQ(solution.intrinsics.cy, lens.cy);
  EXPECT_EQ(solution.intrinsics.k1, lens.k1);
  EXPECT_EQ(solution.intrinsics.k2, lens.k2);
  EXPECT_EQ(solution.intrinsics.k3, lens.k3);
  EXPECT_EQ(solution.intrinsics.p1, lens.p1);
  EXPECT_EQ(solution.intrinsics.p2, lens.p2);
  EXPECT_EQ(solution.intrinsics.width, lens.width);
  EXPECT_EQ(solution.intrinsics.height, lens.height);
  EXPECT_EQ(solution.intrinsics.rollingShutterLineTimeNs, lens.rollingShutterLineTimeNs);
  EXPECT_EQ(solution.intrinsics.estimated, lens.estimated);
}

/**
 * The closing pair is used: a drift every consecutive pair shares comes out.
 *
 * Every pair claims 0.2 degrees too much turn, so a chain arrives 2.4 degrees from where it began.
 * A ring cannot close on those, and the answer that disagrees with each equally is the truth — the
 * uniform drift the solver's own test also uses, here to show the engine hands the closing pair on.
 */
TEST_F(Refine, TheClosingPairTakesOutACommonDrift) {
  const std::vector<Quat> truth = Ring();
  std::vector<PairwiseResult> pairs = RingPairs(truth);
  for (PairwiseResult& pair : pairs) {
    pair.relativeRotation = Normalize(Multiply(pair.relativeRotation, AboutY(0.2)));
  }

  const Result<GlobalSolution> solved = engine_.Refine(pairs, PriorsOut(truth), Lens());
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  const test::RotationScore score = test::ScoreRotations(solved.value.rotations, truth);
  ASSERT_TRUE(score.valid);
  EXPECT_LT(score.maxDeg, 0.01);
  EXPECT_NEAR(solved.value.medianEdgeErrorDeg, 0.2, 0.01) << "each pair is left its own 0.2 out";
  EXPECT_NEAR(solved.value.maxEdgeErrorDeg, 0.2, 0.01);

  // Without the closing pair the drift has nowhere to go.
  pairs.pop_back();
  const Result<GlobalSolution> open = engine_.Refine(pairs, PriorsOut(truth), Lens());
  ASSERT_TRUE(open.ok());
  const test::RotationScore drifted = test::ScoreRotations(open.value.rotations, truth);
  ASSERT_TRUE(drifted.valid);
  EXPECT_GT(drifted.maxDeg, 0.5);
}

/**
 * Pairs count by their inliers, so of two that disagree the better-supported one wins.
 *
 * Two frames and two pairs between them, one claiming 30 degrees on 300 inliers of 400 and one
 * claiming 38 on 100 of 1000. Weighed by inliers the answer sits a quarter of the way from 30 to
 * 38, at 32; equally, at 34; by correspondences, near 35.7. The pairs are then left 2 and 6
 * degrees out, which is what the median and the worst of the edge errors have to say apart.
 */
TEST_F(Refine, APairCountsForItsInliers) {
  const std::vector<Quat> truth{AboutY(0.0), AboutY(30.0)};
  const std::vector<Quat> other{AboutY(0.0), AboutY(38.0)};
  const std::vector<PairwiseResult> pairs{Pair(0, 1, truth, 300, 400), Pair(0, 1, other, 100, 1000)};
  // Frame 1 has no prior, so it is placed through the pairs alone.
  const std::vector<FramePrior> priors{Prior(0, AboutY(0.0)), NoPrior(1)};

  const Result<GlobalSolution> solved = engine_.Refine(pairs, priors, Lens());
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  ASSERT_EQ(solved.value.rotations.size(), 2u);
  const Quat between = Multiply(Conjugate(solved.value.rotations[0]), solved.value.rotations[1]);
  EXPECT_NEAR(SeparationDeg(between, Quat{}), 32.0, 0.01);
  EXPECT_NEAR(solved.value.medianEdgeErrorDeg, 4.0, 0.01) << "2 and 6 have a median of 4";
  EXPECT_NEAR(solved.value.maxEdgeErrorDeg, 6.0, 0.01);
  EXPECT_EQ(solved.value.priorsUsed, 1);
}

/**
 * What an unaccepted pair carries neither moves the answer nor refuses it.
 *
 * `accepted` false is a minority of the correspondences agreeing (ADR 0056); the solve leaves it
 * out. Here one says the two frames are a half turn apart, and one carries no rotation at all —
 * which the solver would refuse the whole input over if it were handed it, at any weight.
 */
TEST_F(Refine, AnUnacceptedPairIsLeftOut) {
  const std::vector<Quat> truth = Ring();
  std::vector<PairwiseResult> pairs = RingPairs(truth);

  PairwiseResult wrong = Pair(0, 6, truth);
  wrong.relativeRotation = Quat{};
  wrong.accepted = false;
  pairs.push_back(wrong);
  PairwiseResult empty = Pair(3, 9, truth);
  empty.relativeRotation = Quat{0, 0, 0, 0};
  empty.inliers = 0;
  empty.correspondences = 0;
  empty.accepted = false;
  pairs.push_back(empty);

  const Result<GlobalSolution> solved = engine_.Refine(pairs, PriorsOut(truth), Lens());
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  EXPECT_EQ(solved.value.edgesUsed, kFrames);
  const test::RotationScore score = test::ScoreRotations(solved.value.rotations, truth);
  ASSERT_TRUE(score.valid);
  EXPECT_LT(score.maxDeg, 0.01);
}

/**
 * A prior nothing anchored is not a prior.
 *
 * `confidence` zero means no reading has ever fixed the orientation (ADR 0041): it is a direction
 * relative to wherever the sensor started. Averaged with anchored priors it would turn the whole
 * reconstruction toward that accident. Here one such prior is forty-five degrees out and the
 * answer faces where the other eleven agree, placing its frame through the pairs.
 */
TEST_F(Refine, APriorNothingAnchoredIsNotAPrior) {
  const std::vector<Quat> truth = Ring();
  std::vector<FramePrior> priors = PriorsOut(truth);
  priors[4].pose.orientation =
      Normalize(Multiply(FromAxisAngle(Vec3{0, 0, 1}, 45.0 / kDegPerRad), truth[4]));
  priors[4].pose.confidence = 0.0;
  // Dead reckoning from an absolute reading is worth half and still counts: only zero is no prior.
  priors[7].pose.confidence = 0.5;

  const Result<GlobalSolution> solved = engine_.Refine(RingPairs(truth), priors, Lens());
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  EXPECT_EQ(solved.value.priorsUsed, kFrames - 1);
  EXPECT_EQ(solved.value.frames.size(), truth.size()) << "its frame is placed through the pairs";

  std::vector<FramePrior> anchored = priors;
  anchored.erase(anchored.begin() + 4);
  std::vector<Quat> anchoredTruth = truth;
  anchoredTruth.erase(anchoredTruth.begin() + 4);
  const test::GaugeAlignment agreed = test::BestGaugeAlignment(Orientations(anchored), anchoredTruth);
  const test::RotationScore score = test::ScoreRotations(solved.value.rotations, truth);
  ASSERT_TRUE(agreed.valid && score.valid);
  EXPECT_NEAR(SeparationDeg(score.alignment, agreed.rotation), 0.0, 1e-3);
}

/**
 * What the solve could not place, and what rests on no pixel, are named by frame.
 *
 * Frames 0 to 2 are joined by accepted pairs. Frame 3 has a prior and no pair: placed, on its prior
 * alone. Frames 4 and 5 have no prior, and the accepted pair between them joins them to
 * nothing that has one: both dropped, and absent from both parallel vectors so neither holds a
 * rotation nobody solved for. Their pair is not counted as used — nothing was placed by it.
 *
 * The priors are given as 4, 0, 5, 1, 2, 3, so the dropped frames are first and in the middle
 * rather than only at the end, which is the one place a walk that stops skipping after its first
 * dropped frame would get right.
 */
TEST_F(Refine, DroppedAndPriorOnlyFramesAreNamed) {
  const std::vector<Quat> truth{AboutY(0.0),  AboutY(30.0),  AboutY(60.0),
                                AboutY(90.0), AboutY(120.0), AboutY(150.0)};
  const std::vector<PairwiseResult> pairs{Pair(0, 1, truth), Pair(1, 2, truth), Pair(4, 5, truth)};
  std::vector<FramePrior> priors;
  for (const int i : {4, 0, 5, 1, 2, 3}) priors.push_back(Prior(i, truth[static_cast<size_t>(i)]));
  priors[0] = NoPrior(4);
  priors[2] = NoPrior(5);

  const Result<GlobalSolution> solved = engine_.Refine(pairs, priors, Lens());
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  const GlobalSolution& solution = solved.value;
  ASSERT_EQ(solution.frames.size(), 4u);
  ASSERT_EQ(solution.rotations.size(), 4u);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(solution.frames[static_cast<size_t>(i)], Frame(i));
    EXPECT_NEAR(SeparationDeg(solution.rotations[static_cast<size_t>(i)], truth[static_cast<size_t>(i)]),
                0.0, kSameRotationDeg);
  }
  ASSERT_EQ(solution.droppedFrames.size(), 2u);
  EXPECT_EQ(solution.droppedFrames[0], Frame(4));
  EXPECT_EQ(solution.droppedFrames[1], Frame(5));
  ASSERT_EQ(solution.priorOnlyFrames.size(), 1u);
  EXPECT_EQ(solution.priorOnlyFrames[0], Frame(3));
  EXPECT_EQ(solution.edgesUsed, 2) << "the pair between two dropped frames placed nothing";
  // Frames 0 to 2, and frame 3 on its prior alone — the seam no pixel stands behind is the one
  // this count exists to show. The dropped frames are no piece at all.
  EXPECT_EQ(solution.pieces, 2);
  EXPECT_EQ(solution.priorsUsed, 4);
}

/**
 * A solve that runs out of sweeps says so.
 *
 * Two hundred frames on exact pairs, each prior at a hundredth of an inlier against pairs of a
 * hundred: the slowest bend of a ring that long outlasts the budget (the table beside `kMaxSweeps`).
 * Every other fixture here converges, so without this one `converged` could be a constant.
 */
TEST_F(Refine, ASolveThatRunsOutOfSweepsSaysSo) {
  const std::vector<Quat> truth = Ring(200);
  const Result<GlobalSolution> solved = engine_.Refine(RingPairs(truth), PriorsOut(truth), Lens());
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  EXPECT_FALSE(solved.value.converged);
}

/**
 * A ring the accepted pairs cut in two is two pieces, and the answer says so.
 *
 * Nothing but the priors relates one piece to the other, so where they meet is placed to the
 * priors' accuracy rather than the pixels': with two declined pairs, the two seams of this ring
 * come out near two degrees while every accepted pair is exact. No other field shows it — no frame
 * is dropped, none rests on its prior alone, the edge errors are tiny — so the count of pieces is
 * how a caller knows which kind of reconstruction it holds.
 */
TEST_F(Refine, APairlessSeamIsReportedAsASecondPiece) {
  const std::vector<Quat> truth = Ring();
  std::vector<PairwiseResult> pairs = RingPairs(truth);
  pairs[5].accepted = false;
  pairs[11].accepted = false;

  const Result<GlobalSolution> solved = engine_.Refine(pairs, PriorsOut(truth), Lens());
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  EXPECT_EQ(solved.value.pieces, 2);
  EXPECT_TRUE(solved.value.droppedFrames.empty());
  EXPECT_TRUE(solved.value.priorOnlyFrames.empty());

  const Result<GlobalSolution> whole = engine_.Refine(RingPairs(truth), PriorsOut(truth), Lens());
  ASSERT_TRUE(whole.ok());
  EXPECT_EQ(whole.value.pieces, 1);
}

/**
 * Priors a half turn apart about where a piece sits name its frames as ambiguous, by frame.
 */
TEST_F(Refine, AmbiguousFramesAreNamed) {
  const std::vector<Quat> truth{AboutY(0.0), AboutY(30.0)};
  const std::vector<FramePrior> priors{
      Prior(0, truth[0]),
      Prior(1, Normalize(Multiply(FromAxisAngle(Vec3{1, 0, 0}, std::numbers::pi), truth[1])))};

  const std::vector<PairwiseResult> pairs{Pair(0, 1, truth)};
  const Result<GlobalSolution> solved = engine_.Refine(pairs, priors, Lens());
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  ASSERT_EQ(solved.value.ambiguousFrames.size(), 2u);
  EXPECT_EQ(solved.value.ambiguousFrames[0], Frame(0));
  EXPECT_EQ(solved.value.ambiguousFrames[1], Frame(1));
}

/**
 * Every input that is not a problem is refused, with the code that says which kind.
 *
 * Each case is a valid input with one thing wrong, and the detail is checked as well as the code,
 * because several refusals share `InvalidArgument` and a case another check refuses first would
 * otherwise pass for this one.
 */
TEST_F(Refine, InputThatIsNotAProblemIsRefused) {
  const std::vector<Quat> truth = Ring();
  const std::vector<PairwiseResult> pairs = RingPairs(truth);
  const std::vector<FramePrior> priors = PriorsOut(truth);
  ASSERT_TRUE(engine_.Refine(pairs, priors, Lens()).ok()) << "the base case must be valid";

  const auto refused = [&](const std::vector<PairwiseResult>& p, const std::vector<FramePrior>& q,
                           const Intrinsics& lens, StatusCode code, const std::string& because) {
    const Result<GlobalSolution> answer = engine_.Refine(p, q, lens);
    EXPECT_FALSE(answer.ok()) << because;
    EXPECT_EQ(answer.status.code, code) << because;
    EXPECT_NE(answer.status.detail.find(because), std::string::npos)
        << "refused for \"" << answer.status.detail << "\", not \"" << because << "\"";
  };
  // Pairs that name only frames 0 to 3, so a change to the priors of the others is what is tested.
  const std::vector<PairwiseResult> firstFour{Pair(0, 1, truth), Pair(1, 2, truth),
                                              Pair(2, 3, truth)};

  refused({}, {}, Lens(), StatusCode::InvalidArgument, "no priors");

  std::vector<FramePrior> repeated = priors;
  repeated[5].frame = repeated[4].frame;
  refused(firstFour, repeated, Lens(), StatusCode::InvalidArgument, "has two priors");

  std::vector<FramePrior> unnamed = priors;
  unnamed[6].frame = FrameId{};
  refused(firstFour, unnamed, Lens(), StatusCode::InvalidArgument, "a prior names no frame");

  refused(pairs, priors, Intrinsics{}, StatusCode::InvalidArgument, "not a usable lens");
  // Wrong in one field only, so a check that reads only some of them is not enough.
  Intrinsics oneFieldOut = Lens();
  oneFieldOut.cy = oneFieldOut.height + 1.0;
  refused(pairs, priors, oneFieldOut, StatusCode::InvalidArgument, "not a usable lens");

  std::vector<PairwiseResult> stranger = pairs;
  stranger[3].b = Frame(99);
  refused(stranger, priors, Lens(), StatusCode::InvalidArgument, "names a frame with no prior");

  // Unaccepted too: a frame nobody gave a prior is the caller and the capture disagreeing about
  // which frames exist, not a measurement to disbelieve.
  stranger[3].accepted = false;
  refused(stranger, priors, Lens(), StatusCode::InvalidArgument, "names a frame with no prior");
  // And at the other end, which is looked up separately.
  std::vector<PairwiseResult> strangerFirst = pairs;
  strangerFirst[3].a = Frame(99);
  refused(strangerFirst, priors, Lens(), StatusCode::InvalidArgument, "names a frame with no prior");

  std::vector<PairwiseResult> self = pairs;
  self[3].b = self[3].a;
  refused(self, priors, Lens(), StatusCode::InvalidArgument, "to itself");
  self[3].accepted = false;
  refused(self, priors, Lens(), StatusCode::InvalidArgument, "to itself");

  std::vector<PairwiseResult> notRotation = pairs;
  notRotation[3].relativeRotation = Quat{0, 0, 0, 0};
  refused(notRotation, priors, Lens(), StatusCode::InvalidArgument, "carries no rotation");
  // Its counts written and its rotation not. Were the default the identity, an accepted pair nobody
  // finished would claim at full weight that its frames share an orientation, and on an open chain
  // every figure of the answer would read clean over a reconstruction thirty degrees out.
  PairwiseResult unfinished;
  unfinished.a = pairs[3].a;
  unfinished.b = pairs[3].b;
  unfinished.inliers = 100;
  unfinished.correspondences = 180;
  unfinished.accepted = true;
  std::vector<PairwiseResult> unwritten = pairs;
  unwritten[3] = unfinished;
  refused(unwritten, priors, Lens(), StatusCode::InvalidArgument, "carries no rotation");

  // Counts no engine fills in. Accepted means a share of the correspondences agree (ADR 0056), so
  // an accepted pair with no inliers contradicts itself — and weighed at zero it would be quietly
  // left out, its frames named prior-only or dropped although an accepted pair touches them. Zero
  // correspondences is what `PairwiseResult` calls a result no engine filled in.
  std::vector<PairwiseResult> counts = pairs;
  counts[3].inliers = -1;
  refused(counts, priors, Lens(), StatusCode::InvalidArgument, "counts no engine fills in");
  counts[3].inliers = 0;
  refused(counts, priors, Lens(), StatusCode::InvalidArgument, "counts no engine fills in");
  counts[3].inliers = 100;
  counts[3].correspondences = 0;
  refused(counts, priors, Lens(), StatusCode::InvalidArgument, "counts no engine fills in");
  counts[3].correspondences = 99;
  refused(counts, priors, Lens(), StatusCode::InvalidArgument, "counts no engine fills in");
  // And the boundary is not refused: every correspondence agreeing is what a frame registered
  // against itself answers with, and it is the best pair there is.
  counts[3].correspondences = 100;
  EXPECT_TRUE(engine_.Refine(counts, priors, Lens()).ok()) << "a pair all of whose matches agree";
  counts[3].correspondences = 0;

  // No prior is spelled with `confidence` zero and no other way. A confidence outside [0, 1], or
  // an orientation that is not a rotation where one is claimed, is a defect upstream; read as no
  // prior it would leave `priorsUsed` one short and name no frame.
  for (const double confidence : {-1.0, 1.5, std::nan("")}) {
    std::vector<FramePrior> outOfRange = priors;
    outOfRange[5].pose.confidence = confidence;
    refused(pairs, outOfRange, Lens(), StatusCode::InvalidArgument, "confidence outside");
  }
  for (const Quat& orientation : {Quat{0, 0, 0, 0}, Quat{std::nan(""), 0, 0, 0}}) {
    std::vector<FramePrior> broken = priors;
    broken[5].pose.orientation = orientation;
    refused(pairs, broken, Lens(), StatusCode::InvalidArgument, "an orientation that is not a rotation");
  }
  // At zero confidence the orientation is not read, so one that is not a rotation is no defect.
  std::vector<FramePrior> unread = priors;
  unread[5] = NoPrior(5);
  unread[5].pose.orientation = Quat{0, 0, 0, 0};
  EXPECT_TRUE(engine_.Refine(pairs, unread, Lens()).ok()) << "an unset pose is no prior";

  // Nothing measured which way the camera pointed: the same condition `ArmBurst` refuses, and with
  // the same code, since the remedy is the sensor and not the pixels.
  std::vector<FramePrior> unanchored = priors;
  for (FramePrior& prior : unanchored) prior.pose.confidence = 0.0;
  refused(pairs, unanchored, Lens(), StatusCode::FailedPrecondition, "no prior is");
  // But a broken prior among them is the caller's arithmetic, not the sensor.
  std::vector<FramePrior> brokenAmongUnanchored = unanchored;
  brokenAmongUnanchored[5].pose.confidence = std::nan("");
  refused(pairs, brokenAmongUnanchored, Lens(), StatusCode::InvalidArgument, "confidence outside");

  // A malformed pair is the caller's defect whatever the priors say: with none of them anchored it
  // is still refused as what it is, not as the capture condition.
  refused(stranger, unanchored, Lens(), StatusCode::InvalidArgument, "names a frame with no prior");
  refused(counts, unanchored, Lens(), StatusCode::InvalidArgument, "counts no engine fills in");
}

}  // namespace
}  // namespace sphanorama
