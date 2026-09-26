// `IRegistrationEngine::Refine` over pairs and priors built by hand, so every answer is known.
//
// Apart from the engine's other tests because it needs no pixels: the pairs are the rotations an
// exact `EstimatePairwise` would have answered with, and the registered frames' own accuracy is
// `registration_accuracy_test.cpp`'s job.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <tuple>
#include <vector>

#include "engines/registration_engine/feature_registration_engine.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "support/match_noise.h"
#include "support/rotation_scoring.h"
#include "support/same_rotation.h"
#include "utilities/camera_model.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

using test::kSameRotationDeg;

constexpr double kDegPerRad = 180.0 / std::numbers::pi;
constexpr int kFrames = 12;

Quat AboutY(double degrees) { return FromAxisAngle(Vec3{0, 1, 0}, degrees / kDegPerRad); }
Quat AboutX(double degrees) { return FromAxisAngle(Vec3{1, 0, 0}, degrees / kDegPerRad); }

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

  // Passed through, every field of it: these pairs carry no matches, so nothing can fit the focal
  // length (ADR 0066).
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
  // Dead reckoning from an absolute reading is worth half and still counts, and so does the least
  // confidence above zero: only zero is no prior, so no threshold between them can pass for it.
  priors[7].pose.confidence = 0.5;
  priors[8].pose.confidence = std::numeric_limits<double>::denorm_min();

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
  // How sure a lens is must be a figure: no figure is infinity, a guess, and nothing is surer than
  // zero.
  for (const double unsure : {std::numeric_limits<double>::quiet_NaN(), -0.001}) {
    Intrinsics claimed = Lens();
    claimed.focalUncertainty = unsure;
    refused(pairs, priors, claimed, StatusCode::InvalidArgument, "focal uncertainty");
  }
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

// A lens with nothing to correct but its focal length, which is the only field `Refine` fits.
Intrinsics TrueLens() {
  Intrinsics lens;
  lens.fx = 500;
  lens.fy = 500;
  lens.cx = 320;
  lens.cy = 240;
  lens.width = 640;
  lens.height = 480;
  return lens;
}

Intrinsics Scaled(Intrinsics lens, double scale) {
  lens.fx *= scale;
  lens.fy *= scale;
  return lens;
}

// The matches an exact `EstimatePairwise` would carry: a grid of pixels in `a`, through the true
// lens to a direction, into `b`'s camera by the true relative rotation, and back out through the
// same lens. Only the ones that land inside `b` are kept, as a real overlap would.
//
// The pair's own rotation is left half a degree out, as one fitted under another lens would be,
// so an answer that kept it rather than refitting from the matches cannot pass for one that did.
PairwiseResult Matched(int a, int b, const std::vector<Quat>& truth, const Intrinsics& lens) {
  PairwiseResult pair = Pair(a, b, truth);
  const Quat exact = pair.relativeRotation;
  for (int row = 1; row < 12; ++row) {
    for (int column = 1; column < 16; ++column) {
      const Pixel from{lens.width * column / 16.0, lens.height * row / 12.0};
      const UnprojectedDirection direction = Unproject(lens, from);
      if (!direction.valid) continue;
      const ProjectedPixel to = Project(lens, Rotate(exact, direction.direction));
      if (!to.valid || to.pixel.x < 0 || to.pixel.y < 0 || to.pixel.x >= lens.width ||
          to.pixel.y >= lens.height) {
        continue;
      }
      pair.inlierMatches.push_back(
          PixelMatch{static_cast<float>(from.x), static_cast<float>(from.y),
                     static_cast<float>(to.pixel.x), static_cast<float>(to.pixel.y)});
    }
  }
  pair.inliers = static_cast<int32_t>(pair.inlierMatches.size());
  pair.correspondences = pair.inliers + 20;
  pair.relativeRotation = Normalize(Multiply(exact, FromAxisAngle(Vec3{1, 0, 0}, 0.5 / kDegPerRad)));
  return pair;
}

std::vector<PairwiseResult> MatchedRing(const std::vector<Quat>& truth, const Intrinsics& lens) {
  std::vector<PairwiseResult> pairs;
  const int n = static_cast<int>(truth.size());
  for (int i = 0; i < n; ++i) pairs.push_back(Matched(i, (i + 1) % n, truth, lens));
  return pairs;
}

/**
 * A focal length eight percent out is fitted from a ring of exact matches, and only the focal
 * length moves (ADR 0066).
 *
 * Eight percent both ways, because a search that only ever looks one way from its start passes one
 * of them. And a ring turning about the horizontal axis as well as one turning about the vertical:
 * a turn about the vertical moves the image sideways and sees only `fx`, so a search that scaled
 * `fx` alone passed on that ring by itself. The matches are exact, so what is left is the search's own tolerance; the rotations
 * come back to the truth because they are refitted under the fitted lens, which a solve that fitted
 * the lens and kept the pairs' own rotations would not do.
 *
 * **And a loop need not wrap.** Three frames turning about one axis were expected to close under any
 * focal length, since scaling three angles that sum to zero leaves them summing to zero — and this
 * test first asserted the triangle was passed through. It came back fitted, to 499.9967 of 500:
 * under a pinhole a turn moves a pixel by the tangent of its angle, not by the angle, so refitted
 * under the wrong focal length a thirty-degree pair and a sixty-degree one are not scaled alike
 * — 2 x 32.324 against 64.220 in a reviewer's rebuild — and the triangle stops closing. Not by
 * tilting: the refitted axes stay exactly vertical. A lone triangle sees the focal length only that
 * faintly, though, and read through a lens model good to a thousandth of the focal length it is not
 * precise enough to take (`ALoopTooRoughToFitDoesNotOverrideTheLensHandedIn`); a grid of such loops, none wrapping,
 * is.
 */
TEST_F(Refine, AFocalLengthOutIsFittedFromTheRing) {
  const std::vector<Quat> truth = Ring();
  const Intrinsics lens = TrueLens();
  const std::vector<PairwiseResult> ring = MatchedRing(truth, lens);
  for (const PairwiseResult& pair : ring) ASSERT_GE(pair.inlierMatches.size(), 20u);
  // Two rows of four, thirty degrees apart each way: four loops, and none of them wraps.
  std::vector<Quat> grid;
  for (int row = 0; row < 2; ++row) {
    for (int column = 0; column < 4; ++column) {
      grid.push_back(Normalize(Multiply(AboutY(30.0 * column), AboutX(-30.0 * row))));
    }
  }
  std::vector<PairwiseResult> gridPairs;
  for (int row = 0; row < 2; ++row) {
    for (int column = 0; column < 3; ++column) {
      gridPairs.push_back(Matched(row * 4 + column, row * 4 + column + 1, grid, lens));
    }
  }
  for (int column = 0; column < 4; ++column) {
    gridPairs.push_back(Matched(column, 4 + column, grid, lens));
  }

  std::vector<Quat> tumbling;
  for (int i = 0; i < kFrames; ++i) tumbling.push_back(AboutX(360.0 * i / kFrames));
  // A lens whose two focal lengths differ, so fitting one of them and copying it is not enough.
  Intrinsics tall = lens;
  tall.fy = 540;
  const std::vector<PairwiseResult> pitched = MatchedRing(tumbling, tall);
  for (const PairwiseResult& pair : pitched) ASSERT_GE(pair.inlierMatches.size(), 20u);
  // And a lens that distorts in every term, so a trial that dropped the distortion, or a fit that
  // returned the lens without any one of them, is wrong by more than the tolerance.
  Intrinsics barrel = lens;
  barrel.k1 = -0.1;
  barrel.k2 = 0.02;
  barrel.k3 = -0.003;
  barrel.p1 = 4e-4;
  barrel.p2 = -3e-4;
  const std::vector<PairwiseResult> distorted = MatchedRing(truth, barrel);
  for (const PairwiseResult& pair : distorted) ASSERT_GE(pair.inlierMatches.size(), 20u);
  // And a pair with exactly three matches, the fewest a fit is taken on, spread across the overlap.
  std::vector<PairwiseResult> three = ring;
  const std::vector<PixelMatch> all = three[4].inlierMatches;
  three[4].inlierMatches = {all.front(), all[all.size() / 2], all.back()};
  three[4].inliers = 3;

  const struct {
    std::vector<PairwiseResult> pairs;
    std::vector<Quat> truth;
    Intrinsics lens;
    int placed;
  } shapes[] = {{ring, truth, lens, kFrames},
                {gridPairs, grid, lens, 8},
                {pitched, tumbling, tall, kFrames},
                {distorted, truth, barrel, kFrames},
                {three, truth, lens, kFrames}};
  for (const auto& [pairs, truth, lens, placed] : shapes)
  for (const double scale : {0.92, 1.08}) {
    Intrinsics initial = Scaled(lens, scale);
    initial.rollingShutterLineTimeNs = 15000;
    const Result<GlobalSolution> solved = engine_.Refine(pairs, PriorsOut(truth), initial);
    ASSERT_TRUE(solved.ok()) << solved.status.detail;
    const GlobalSolution& solution = solved.value;
    EXPECT_TRUE(solution.lensFitted) << scale;
    EXPECT_TRUE(solution.intrinsics.estimated) << scale;
    // Within 9.1e-6 of the truth in every case measured, which is the search's own resolution:
    // three times that, so a tolerance ten times looser does not pass.
    EXPECT_NEAR(solution.intrinsics.fx / lens.fx, 1.0, 3e-5) << scale;
    EXPECT_NEAR(solution.intrinsics.fy / lens.fy, 1.0, 3e-5) << scale;
    EXPECT_EQ(solution.intrinsics.k1, initial.k1) << scale;
    EXPECT_EQ(solution.intrinsics.k2, initial.k2) << scale;
    EXPECT_EQ(solution.intrinsics.k3, initial.k3) << scale;
    EXPECT_EQ(solution.intrinsics.p1, initial.p1) << scale;
    EXPECT_EQ(solution.intrinsics.p2, initial.p2) << scale;
    EXPECT_EQ(solution.intrinsics.cx, initial.cx);
    EXPECT_EQ(solution.intrinsics.cy, initial.cy);
    EXPECT_EQ(solution.intrinsics.width, initial.width);
    EXPECT_EQ(solution.intrinsics.height, initial.height);
    EXPECT_EQ(solution.intrinsics.rollingShutterLineTimeNs, initial.rollingShutterLineTimeNs);

    // Scored over the frames the pairs place.
    const std::vector<Quat> placedTruth(truth.begin(), truth.begin() + placed);
    const std::vector<Quat> placedSolved(solution.rotations.begin(),
                                         solution.rotations.begin() + placed);
    const test::RotationScore shape = test::ScoreRotations(placedSolved, placedTruth);
    ASSERT_TRUE(shape.valid);
    EXPECT_LT(shape.maxDeg, 0.01) << scale;
    EXPECT_LT(solution.maxEdgeErrorDeg, 0.01) << scale;
  }

  // Just inside each end of the bracket, so a search narrower than 0.7 to 1.4 is caught here, as a
  // wider one is by the rows just past it in `ALensNothingCanSeeIsPassedThrough`.
  for (const double scale : {1.0 / 0.705, 1.0 / 1.39}) {
    const Result<GlobalSolution> solved = engine_.Refine(ring, PriorsOut(truth), Scaled(lens, scale));
    ASSERT_TRUE(solved.ok()) << solved.status.detail;
    EXPECT_TRUE(solved.value.lensFitted) << scale;
    EXPECT_NEAR(solved.value.intrinsics.fx / lens.fx, 1.0, 3e-5) << scale;
  }
}

/**
 * Where nothing can see the focal length, the lens comes back as it was given, every field of it,
 * and says it was not fitted.
 *
 * An open chain agrees with itself under any focal length. A pair with no matches cannot be
 * refitted under another lens at all, and one with two is refused as surely: Kabsch fits a rotation
 * from two, but with nothing to check them against. And an answer outside the range the search may
 * look in is the search reporting its own bracket, not the lens.
 */
TEST_F(Refine, ALensNothingCanSeeIsPassedThrough) {
  const std::vector<Quat> truth = Ring();
  const Intrinsics lens = TrueLens();
  const std::vector<PairwiseResult> ring = MatchedRing(truth, lens);

  std::vector<PairwiseResult> chain = ring;
  chain.pop_back();
  // A pair measured twice agrees with itself under any focal length, so it is no loop.
  std::vector<PairwiseResult> twice = chain;
  twice.push_back(chain[3]);
  // And measured the other way round, which is the same two frames.
  std::vector<PairwiseResult> reversed = chain;
  reversed.push_back(Matched(4, 3, truth, lens));
  std::vector<PairwiseResult> unmatched = ring;
  unmatched[4].inlierMatches.clear();
  unmatched[4].inliers = 100;
  unmatched[4].correspondences = 180;
  // Two matches, which `KabschRotation` would take, so only `Refine`'s own floor refuses them.
  std::vector<PairwiseResult> twoMatches = ring;
  twoMatches[4].inlierMatches.resize(2);
  twoMatches[4].inliers = 2;
  // And beside a ring that closes without it, so a thin pair that was dropped rather than refused
  // would leave a loop to fit on.
  std::vector<PairwiseResult> twoMatchChord = ring;
  twoMatchChord.push_back(Matched(0, 2, truth, lens));
  twoMatchChord.back().inlierMatches.resize(2);
  twoMatchChord.back().inliers = 2;

  // An open chain beside a closed triangle of frames nothing anchors. The solve leaves the triangle
  // unplaced, so the loop is not one the fit may score: counted, it let the chain's priors fit the
  // lens to 502.7 of 500 under an earlier rule (a reviewer's probe) — a least that is the priors'
  // rather than the pixels', which is what the loop check exists to refuse.
  std::vector<Quat> islandTruth = truth;
  islandTruth.insert(islandTruth.end(), truth.begin(), truth.begin() + 3);
  std::vector<PairwiseResult> chainAndIsland = chain;
  chainAndIsland.push_back(Matched(kFrames, kFrames + 1, islandTruth, lens));
  chainAndIsland.push_back(Matched(kFrames + 1, kFrames + 2, islandTruth, lens));
  chainAndIsland.push_back(Matched(kFrames + 2, kFrames, islandTruth, lens));
  std::vector<FramePrior> islandPriors = PriorsOut(truth);
  for (int i = kFrames; i < kFrames + 3; ++i) islandPriors.push_back(NoPrior(i));

  // A loop of turns about the viewing axis: every pixel wheels about the centre by the same angle
  // under any focal length, so the cost is flat and there is no least to find.
  std::vector<Quat> rolling;
  for (int i = 0; i < kFrames; ++i) {
    rolling.push_back(FromAxisAngle(Vec3{0, 0, 1}, 2.0 * std::numbers::pi * i / kFrames));
  }
  const std::vector<PairwiseResult> rolled = MatchedRing(rolling, lens);

  const std::vector<FramePrior> priors = PriorsOut(truth);
  const struct {
    std::vector<PairwiseResult> pairs;
    std::vector<FramePrior> priors;
    double scale;
    const char* why;
  } cases[] = {{chain, priors, 1.08, "an open chain"},
               {twice, priors, 1.08, "an open chain with a pair measured twice"},
               {reversed, priors, 1.08, "an open chain with a pair measured both ways"},
               {unmatched, priors, 1.08, "a pair with no matches"},
               {twoMatches, priors, 1.08, "a pair with two matches"},
               {twoMatches, priors, 0.92, "a pair with two matches, short"},
               {twoMatchChord, priors, 1.08, "a ring with a two-match chord"},
               {chainAndIsland, islandPriors, 1.08, "an open chain beside an unplaced loop"},
               {chainAndIsland, islandPriors, 0.92, "an open chain beside an unplaced loop, short"},
               {rolled, PriorsOut(rolling), 1.08, "a loop about the viewing axis"},
               // Each end of the bracket on its own, since each half of the rise test looks only
               // one way: at a least on the long end the cost short of it rises and past it falls.
               // Nothing else refuses these — the lens handed in is hundreds of the least's
               // standard deviations from it — and the rows further out are the same refusal from
               // a truth well past the end.
               {ring, priors, 1.0 / 1.41, "a focal length just past the long end of the search"},
               {ring, priors, 1.0 / 0.695, "a focal length just past the short end of the search"},
               {ring, priors, 1.0 / 1.6, "a focal length past the long end of the search"},
               {ring, priors, 1.0 / 0.6, "a focal length past the short end of the search"},
               {ring, priors, 2.5, "a focal length far outside the search"}};
  for (const auto& [pairs, poses, scale, why] : cases) {
    for (const bool estimated : {true, false}) {
      // Every field away from its default, so passing each through is told apart from resetting
      // it — the distortion small enough that the fit decision is still about the focal length.
      Intrinsics initial = Scaled(lens, scale);
      initial.k1 = -1e-3;
      initial.k2 = 2e-4;
      initial.k3 = -3e-5;
      initial.p1 = 4e-5;
      initial.p2 = -5e-5;
      initial.rollingShutterLineTimeNs = 15000;
      initial.estimated = estimated;
      const Result<GlobalSolution> solved = engine_.Refine(pairs, poses, initial);
      ASSERT_TRUE(solved.ok()) << why << ": " << solved.status.detail;
      EXPECT_FALSE(solved.value.lensFitted) << why;
      const Intrinsics& out = solved.value.intrinsics;
      EXPECT_EQ(out.fx, initial.fx) << why;
      EXPECT_EQ(out.fy, initial.fy) << why;
      EXPECT_EQ(out.cx, initial.cx) << why;
      EXPECT_EQ(out.cy, initial.cy) << why;
      EXPECT_EQ(out.k1, initial.k1) << why;
      EXPECT_EQ(out.k2, initial.k2) << why;
      EXPECT_EQ(out.k3, initial.k3) << why;
      EXPECT_EQ(out.p1, initial.p1) << why;
      EXPECT_EQ(out.p2, initial.p2) << why;
      EXPECT_EQ(out.width, initial.width) << why;
      EXPECT_EQ(out.height, initial.height) << why;
      EXPECT_EQ(out.rollingShutterLineTimeNs, initial.rollingShutterLineTimeNs) << why;
      EXPECT_EQ(out.estimated, estimated) << why;
      // No least to report a spread for — at an end of the bracket too, however sharply the cost
      // falls toward it. The rolled loop's cost is flat but for rounding, which can rise either side
      // of some scale; what it reports then is no spread a fit could be taken on.
      if (std::string(why) == "a loop about the viewing axis") {
        EXPECT_TRUE(std::isinf(solved.value.focalSpread) || solved.value.focalSpread > 0.1)
            << why << ": " << solved.value.focalSpread;
      } else {
        EXPECT_TRUE(std::isinf(solved.value.focalSpread)) << why << ": " << solved.value.focalSpread;
        EXPECT_TRUE(std::isinf(solved.value.focalModelError)) << why << ": " << solved.value.focalModelError;
      }

      // And the rotations are the solve of the pairs' own rotations, not a trial's: the same pairs
      // with no matches to refit from, which cannot reach the search at all, give them exactly.
      std::vector<PairwiseResult> unrefittable = pairs;
      for (PairwiseResult& pair : unrefittable) pair.inlierMatches.clear();
      const Result<GlobalSolution> own = engine_.Refine(unrefittable, poses, initial);
      ASSERT_TRUE(own.ok()) << why << ": " << own.status.detail;
      ASSERT_EQ(solved.value.rotations.size(), own.value.rotations.size()) << why;
      for (size_t i = 0; i < own.value.rotations.size(); ++i) {
        const Quat& got = solved.value.rotations[i];
        const Quat& want = own.value.rotations[i];
        EXPECT_TRUE(got.w == want.w && got.x == want.x && got.y == want.y && got.z == want.z)
            << why << ", frame " << i;
      }
      EXPECT_EQ(solved.value.medianEdgeErrorDeg, own.value.medianEdgeErrorDeg) << why;
    }
  }
}

// The shapes the noisy tests share: the ring, the ring with its closing pair swapped for one that
// skips a frame — its one loop thirty and thirty degrees against sixty — a triangle, and two rows of
// four thirty degrees apart each way, four loops none of which wraps.
struct NoisyShapes {
  std::vector<Quat> truth = Ring();
  std::vector<Quat> grid;
  std::vector<PairwiseResult> ring, skipping, triangle, gridPairs;
  NoisyShapes() {
    const Intrinsics lens = TrueLens();
    ring = MatchedRing(truth, lens);
    skipping = ring;
    skipping.pop_back();
    skipping.push_back(Matched(0, 2, truth, lens));
    triangle = {Matched(0, 1, truth, lens), Matched(1, 2, truth, lens), Matched(2, 0, truth, lens)};
    for (int row = 0; row < 2; ++row) {
      for (int column = 0; column < 4; ++column) {
        grid.push_back(Normalize(Multiply(AboutY(30.0 * column), AboutX(-30.0 * row))));
      }
    }
    for (int row = 0; row < 2; ++row) {
      for (int column = 0; column < 3; ++column) {
        gridPairs.push_back(Matched(row * 4 + column, row * 4 + column + 1, grid, lens));
      }
    }
    for (int column = 0; column < 4; ++column) {
      gridPairs.push_back(Matched(column, 4 + column, grid, lens));
    }
  }
};

/**
 * A loop too small to see the focal length through the pairs' noise is not a fit, and a ring or a
 * grid carrying the same noise is.
 *
 * Handed the right lens, with 0.8 px of noise on every match — ORB's pair residual on the photograph
 * ring — the skipping ring was fitted every time and up to 1.5% out, the rotations ten times worse
 * than the right lens gave them and the edge error reading clean (round 3). A rule that asked the
 * cost to double within half a percent of its least then took it in six seeds of two hundred, seed
 * 101 among them at 2.2% out (round 4): the cost at the least is the loops' residual, which is
 * independent of how far the noise moved the least, so judging by it selects nothing. Judged by the
 * precision the pairs' own residuals give, the skipping ring and the triangle sit at 0.39% or worse
 * at 0.4 px and the grid at 0.15% or better at 0.8, in every seed — so a handful of seeds decides it,
 * and seed 101 is here for the record. The ring and the grid are the other half: a rule that refused
 * every noisy fit would pass the first half alone.
 */
TEST_F(Refine, ALoopTooSmallToSeeThroughTheNoiseIsNotAFit) {
  const NoisyShapes shapes;
  const Intrinsics lens = TrueLens();
  for (const uint32_t seed : {1u, 2u, 3u, 4u, 5u, 6u, 101u}) {
    for (const auto& [pairs, poses, sigma, fits, why] :
         {std::tuple{shapes.skipping, shapes.truth, 0.8, false, "a ring whose one loop skips a frame"},
          std::tuple{shapes.skipping, shapes.truth, 0.4, false, "the same, at 0.4 px"},
          std::tuple{shapes.triangle, shapes.truth, 0.8, false, "a triangle"},
          std::tuple{shapes.triangle, shapes.truth, 0.4, false, "a triangle, at 0.4 px"},
          std::tuple{shapes.gridPairs, shapes.grid, 0.8, true, "a grid"},
          // Precise to 0.33% at the best: a spread that counted every axis of a pair's rotation as
          // well measured as the two across its view read it three times tighter, and took it.
          std::tuple{shapes.gridPairs, shapes.grid, 2.0, false, "a grid, at 2 px"},
          std::tuple{shapes.ring, shapes.truth, 0.8, true, "a ring"}}) {
      const Result<GlobalSolution> solved =
          engine_.Refine(test::WithNoise(pairs, sigma, seed), PriorsOut(poses), lens);
      ASSERT_TRUE(solved.ok()) << why << ": " << solved.status.detail;
      EXPECT_EQ(solved.value.lensFitted, fits) << why << ", seed " << seed;
      if (fits) {
        // 0.39% in the worst of two hundred of the grid's seeds, 0.03% in the ring's.
        EXPECT_NEAR(solved.value.intrinsics.fx / lens.fx, 1.0, 0.006) << why << ", seed " << seed;
      } else {
        EXPECT_EQ(solved.value.intrinsics.fx, lens.fx) << why << ", seed " << seed;
      }
    }
  }
}

/**
 * A precision read from each pair's own noise, not the pairs' pooled.
 *
 * One pair of the ring thinned to fifteen matches at five times the others' noise: the solve puts
 * the loop's misfit where the pairs say least, and a pooled residual diluted that pair's noise with
 * the rest's, so the least wandered three times further than the spread it reported — and at twenty
 * times the noise, a spread read from the pool fitted every seed, up to 0.8% out, where the pair's
 * own refuses every one (round 7).
 */
TEST_F(Refine, ANoisyThinPairIsWeighedByItsOwnNoise) {
  const NoisyShapes shapes;
  const Intrinsics lens = TrueLens();
  const auto withThin = [&](size_t count, double sigma, uint32_t seed) {
    std::vector<PairwiseResult> pairs = test::WithNoise(shapes.ring, 0.8, seed);
    PairwiseResult thin = shapes.ring[0];
    thin.inlierMatches.resize(count);
    thin.inliers = static_cast<int32_t>(count);
    pairs[0] = test::WithNoise({thin}, sigma, seed + 1000)[0];
    return pairs;
  };
  double squares = 0.0;
  for (uint32_t seed = 1; seed <= 20; ++seed) {
    const Result<GlobalSolution> solved =
        engine_.Refine(withThin(15, 4.0, seed), PriorsOut(shapes.truth), lens);
    ASSERT_TRUE(solved.ok()) << solved.status.detail;
    ASSERT_TRUE(solved.value.lensFitted) << "seed " << seed;
    const double z = std::log(solved.value.intrinsics.fx / lens.fx) / solved.value.focalSpread;
    squares += z * z;
  }
  // 0.99 over these twenty, and 3.3 read from the pool.
  EXPECT_LT(std::sqrt(squares / 20.0), 1.5);
  for (const uint32_t seed : {1u, 2u, 3u, 4u, 5u, 6u}) {
    const Result<GlobalSolution> solved =
        engine_.Refine(withThin(15, 16.0, seed), PriorsOut(shapes.truth), lens);
    ASSERT_TRUE(solved.ok()) << solved.status.detail;
    EXPECT_FALSE(solved.value.lensFitted) << "seed " << seed;
  }

  // But never quieter than the pool: three matches leave a pair three degrees of freedom, and in
  // these seeds its own residual read so low that, taken alone, it put the least eight of its
  // spreads out.
  for (const uint32_t seed : {18u, 100u}) {
    std::vector<PairwiseResult> pairs = test::WithNoise(shapes.ring, 0.8, seed);
    pairs[0].inlierMatches.resize(3);
    pairs[0].inliers = 3;
    const Result<GlobalSolution> solved = engine_.Refine(pairs, PriorsOut(shapes.truth), lens);
    ASSERT_TRUE(solved.ok()) << solved.status.detail;
    ASSERT_TRUE(solved.value.lensFitted) << "seed " << seed;
    EXPECT_LT(std::abs(std::log(solved.value.intrinsics.fx / lens.fx) / solved.value.focalSpread), 4.0)
        << "seed " << seed;
  }
}

/**
 * A pair measured both ways is one measurement, so it does not make the least look more precise.
 *
 * The grid at 1.5 px is precise to about a quarter of a percent, too loose to be taken over the
 * right lens. With every pair also carried the other way round — the same matches, swapped — its
 * noise was summed as two independent noises and read √2 tighter, under the threshold (round 5).
 */
TEST_F(Refine, APairMeasuredBothWaysIsNotTwiceAsSure) {
  const NoisyShapes shapes;
  const Intrinsics lens = TrueLens();
  for (const uint32_t seed : {1u, 2u, 3u, 4u, 5u, 6u}) {
    std::vector<PairwiseResult> pairs = test::WithNoise(shapes.gridPairs, 1.5, seed);
    const Result<GlobalSolution> once = engine_.Refine(pairs, PriorsOut(shapes.grid), lens);
    ASSERT_TRUE(once.ok()) << once.status.detail;
    ASSERT_FALSE(once.value.lensFitted) << "the premise: once is too loose, seed " << seed;

    const size_t count = pairs.size();
    for (size_t i = 0; i < count; ++i) {
      PairwiseResult back = pairs[i];
      std::swap(back.a, back.b);
      back.relativeRotation = Conjugate(back.relativeRotation);
      for (PixelMatch& match : back.inlierMatches) {
        std::swap(match.ax, match.bx);
        std::swap(match.ay, match.by);
      }
      pairs.push_back(back);
    }
    const Result<GlobalSolution> twice = engine_.Refine(pairs, PriorsOut(shapes.grid), lens);
    ASSERT_TRUE(twice.ok()) << twice.status.detail;
    EXPECT_FALSE(twice.value.lensFitted) << "seed " << seed;
    // And no looser either: the same noise, measured twice, is exactly as precise as measured once
    // — within 0.03% in every seed.
    EXPECT_NEAR(twice.value.focalSpread / once.value.focalSpread, 1.0, 0.01) << "seed " << seed;
  }
}

/**
 * The spread `Refine` reports is the scatter the least actually has, and a measured figure.
 *
 * Two halves, because each catches what the other cannot. Over twenty seeds of each fitted shape,
 * the least's error in reported spreads has a root mean square near one — 0.88 over these sixty
 * fits, the grid's at 1.07, the ring's 0.80 and the uneven ring's 0.73, and 0.83 to 0.95 each over
 * two hundred seeds — so a spread wrong by half or double is caught. The skipping ring is refused,
 * too rough to take, and its spread is held all the same. But a band that wide passes a spread 10 or 20% off, and the parts it is built from each
 * move it by that much: three coordinates a match rather than two moved the grid's 19%, a pair's
 * information gathered in its first frame rather than its second 11%, its error vector in the
 * other frame 11%, and the pairs' weights dropped moved the skipping ring's 9% (round 5). So one
 * seed of each is also held to within 3% of what it measured.
 *
 * And the spread is reported where the fit is refused, since a caller weighing lenses wants to see
 * how near a refused one came, and is infinite where no least was reached at all — including a
 * search that stopped at an end of its bracket, which `ALensNothingCanSeeIsPassedThrough` asserts.
 */
TEST_F(Refine, TheSpreadReportedIsTheLeastsOwnScatter) {
  const NoisyShapes shapes;
  const Intrinsics lens = TrueLens();
  // Every other pair of the ring thinned to nine matches, so the pairs' weights differ tenfold.
  std::vector<PairwiseResult> uneven = shapes.ring;
  for (size_t i = 0; i < uneven.size(); i += 2) {
    uneven[i].inlierMatches.resize(9);
    uneven[i].inliers = 9;
  }
  const struct {
    std::vector<PairwiseResult> pairs;
    std::vector<Quat> poses;
    double sigma;
    double scale;
    bool fits;
    double firstSeedSpread;
    const char* why;
  } rows[] = {{shapes.ring, shapes.truth, 0.8, 1.0, true, 0.00013976, "a ring"},
              {shapes.gridPairs, shapes.grid, 0.8, 1.0, true, 0.00146026, "a grid"},
              {shapes.skipping, shapes.truth, 0.8, 1.08, false, 0.00896840,
               "a skipping ring handed a lens 8% out, too rough to take"},
              {uneven, shapes.truth, 0.8, 1.0, true, 0.00081659, "a ring of uneven pairs"},
              {shapes.gridPairs, shapes.grid, 1.25, 1.0, false, 0.00228224,
               "a grid too noisy to take"}};

  double squares = 0.0;
  int fitted = 0;
  for (const auto& row : rows) {
    for (uint32_t seed = 1; seed <= 20; ++seed) {
      const Result<GlobalSolution> solved = engine_.Refine(
          test::WithNoise(row.pairs, row.sigma, seed), PriorsOut(row.poses), Scaled(lens, row.scale));
      ASSERT_TRUE(solved.ok()) << row.why << ": " << solved.status.detail;
      const GlobalSolution& solution = solved.value;
      ASSERT_TRUE(std::isfinite(solution.focalSpread)) << row.why << ", seed " << seed;
      EXPECT_EQ(solution.lensFitted, row.fits) << row.why << ", seed " << seed;
      if (seed == 1) {
        EXPECT_NEAR(solution.focalSpread / row.firstSeedSpread, 1.0, 0.03) << row.why;
      }
      if (solution.lensFitted) {
        const double z = std::log(solution.intrinsics.fx / lens.fx) / solution.focalSpread;
        EXPECT_LT(std::abs(z), 4.0) << row.why << ", seed " << seed;
        squares += z * z;
        ++fitted;
      }
    }
  }
  ASSERT_EQ(fitted, 60);
  const double rms = std::sqrt(squares / fitted);
  EXPECT_GT(rms, 0.6);
  EXPECT_LT(rms, 1.15);

  std::vector<PairwiseResult> chain = shapes.ring;
  chain.pop_back();
  const Result<GlobalSolution> open = engine_.Refine(chain, PriorsOut(shapes.truth), lens);
  ASSERT_TRUE(open.ok()) << open.status.detail;
  EXPECT_TRUE(std::isinf(open.value.focalSpread));
  EXPECT_TRUE(std::isinf(open.value.focalModelError));
}

/**
 * A loop that sees the focal length only roughly does not override the lens it is handed, however
 * far from it its least lies — not the right one, and not one 8% out — and nor do many of them.
 *
 * A lens model a little wrong moves a weak loop's least by more than its noise does: such a loop
 * reads the focal length through the curve of the tangent, and reads distortion through the same
 * curve. Handed the right focal length with an unreported k1 of -0.01 — 2.6 px at the corner — the
 * skipping ring and the triangle put their least 1.6% out on exact matches, 5.0 to 5.1% at -0.03,
 * and a rule that took a least far enough from the lens handed in as refuting it fitted there, the
 * rotations seven times worse (round 6). So the lens handed in stands, right or 8% out; correcting a
 * lens a weak loop cannot see precisely is for a capture with the loops to do it, or for the lens a
 * device keeps.
 */
TEST_F(Refine, ALoopTooRoughToFitDoesNotOverrideTheLensHandedIn) {
  const NoisyShapes shapes;
  const Intrinsics lens = TrueLens();
  // Distorted as a phone's residual might be, and handed in without it.
  for (const double k1 : {-0.01, -0.03}) {
    Intrinsics distorted = lens;
    distorted.k1 = k1;
    const std::vector<PairwiseResult> skipping = [&] {
      std::vector<PairwiseResult> ring = MatchedRing(shapes.truth, distorted);
      ring.pop_back();
      ring.push_back(Matched(0, 2, shapes.truth, distorted));
      return ring;
    }();
    const std::vector<PairwiseResult> triangle{Matched(0, 1, shapes.truth, distorted),
                                               Matched(1, 2, shapes.truth, distorted),
                                               Matched(2, 0, shapes.truth, distorted)};
    for (const uint32_t seed : {0u, 1u, 2u, 3u}) {
      for (const auto& pairs : {skipping, triangle}) {
        const std::vector<PairwiseResult> noisy = seed == 0 ? pairs : test::WithNoise(pairs, 0.8, seed);
        const Result<GlobalSolution> solved = engine_.Refine(noisy, PriorsOut(shapes.truth), lens);
        ASSERT_TRUE(solved.ok()) << solved.status.detail;
        EXPECT_FALSE(solved.value.lensFitted) << k1 << ", seed " << seed;
        EXPECT_EQ(solved.value.intrinsics.fx, lens.fx) << k1 << ", seed " << seed;
      }
    }
  }
  // Many weak loops are no stronger than one against a lens model a little wrong: a ring open at one
  // pair with a chord across every other frame has matches enough for its noise to read under 0.17%
  // at 0.4 px, and under that same k1 of -0.01 its least lies 1.8% out, the rotations six times
  // worse. A half-pixel misreading moves it 0.30%, as it does one loop (round 7).
  for (const double k1 : {-0.005, -0.01}) {
    Intrinsics distorted = lens;
    distorted.k1 = k1;
    std::vector<PairwiseResult> chords = MatchedRing(shapes.truth, distorted);
    chords.pop_back();
    for (int32_t i = 0; i + 2 < 12; ++i) chords.push_back(Matched(i, i + 2, shapes.truth, distorted));
    for (const uint32_t seed : {0u, 1u, 2u, 3u}) {
      const std::vector<PairwiseResult> noisy = seed == 0 ? chords : test::WithNoise(chords, 0.4, seed);
      const Result<GlobalSolution> solved = engine_.Refine(noisy, PriorsOut(shapes.truth), lens);
      ASSERT_TRUE(solved.ok()) << solved.status.detail;
      EXPECT_FALSE(solved.value.lensFitted) << "chords, " << k1 << ", seed " << seed;
      // Refused by the model error, not the noise.
      EXPECT_LT(solved.value.focalSpread, 0.002) << "chords, " << k1 << ", seed " << seed;
      EXPECT_GT(solved.value.focalModelError, 0.002) << "chords, " << k1 << ", seed " << seed;
      EXPECT_EQ(solved.value.intrinsics.fx, lens.fx) << "chords, " << k1 << ", seed " << seed;
    }
  }
  // And at the size the page grabs at, twice this lens: the misreading is a fraction of the focal
  // length, not a count of pixels, since a lens's distortion does not change with the size of the
  // frame it is read into. Measured in pixels, it halved here, and the chords were fitted 1.6% out,
  // the rotations six times worse (round 8).
  Intrinsics doubled = lens;
  doubled.fx *= 2.0;
  doubled.fy *= 2.0;
  doubled.cx *= 2.0;
  doubled.cy *= 2.0;
  doubled.width *= 2;
  doubled.height *= 2;
  for (const double k1 : {-0.005, -0.01}) {
    Intrinsics distorted = doubled;
    distorted.k1 = k1;
    std::vector<PairwiseResult> chords = MatchedRing(shapes.truth, distorted);
    chords.pop_back();
    for (int32_t i = 0; i + 2 < 12; ++i) chords.push_back(Matched(i, i + 2, shapes.truth, distorted));
    for (const uint32_t seed : {0u, 1u, 2u}) {
      const std::vector<PairwiseResult> noisy = seed == 0 ? chords : test::WithNoise(chords, 0.4, seed);
      const Result<GlobalSolution> solved = engine_.Refine(noisy, PriorsOut(shapes.truth), doubled);
      ASSERT_TRUE(solved.ok()) << solved.status.detail;
      EXPECT_FALSE(solved.value.lensFitted) << "chords at 1280, " << k1 << ", seed " << seed;
      EXPECT_LT(solved.value.focalSpread, 0.002) << "chords at 1280, " << k1 << ", seed " << seed;
      EXPECT_GT(solved.value.focalModelError, 0.002) << "chords at 1280, " << k1 << ", seed " << seed;
    }
  }
  // A ring has the loops to absorb the same distortion into a focal length a little off and keep its
  // rotations right, and is fitted: 0.24% short, the rotations within a hundredth of a degree.
  Intrinsics distorted = lens;
  distorted.k1 = -0.01;
  const Result<GlobalSolution> ring =
      engine_.Refine(MatchedRing(shapes.truth, distorted), PriorsOut(shapes.truth), lens);
  ASSERT_TRUE(ring.ok()) << ring.status.detail;
  EXPECT_TRUE(ring.value.lensFitted);
  EXPECT_NEAR(ring.value.intrinsics.fx / lens.fx, 1.0, 0.005);
  const test::RotationScore absorbed = test::ScoreRotations(ring.value.rotations, shapes.truth);
  ASSERT_TRUE(absorbed.valid);
  EXPECT_LT(absorbed.maxDeg, 0.01);

  // And a lens 8% out stands too: 400 seeds of the skipping ring passed it through.
  for (const uint32_t seed : {1u, 2u, 3u, 4u, 5u, 6u}) {
    for (const double scale : {0.92, 1.08}) {
      const Result<GlobalSolution> solved = engine_.Refine(test::WithNoise(shapes.skipping, 0.8, seed),
                                                           PriorsOut(shapes.truth), Scaled(lens, scale));
      ASSERT_TRUE(solved.ok()) << solved.status.detail;
      EXPECT_FALSE(solved.value.lensFitted) << scale << ", seed " << seed;
      EXPECT_EQ(solved.value.intrinsics.fx, Scaled(lens, scale).fx) << scale << ", seed " << seed;
    }
  }
}

/**
 * The model error `Refine` reports is how far a lens whose frame corner is misread by a thousandth of
 * the focal length moves the least — measured by misreading it.
 *
 * Rendered through a radial distortion that moves the corner that far either way and handed the lens
 * without it, a ring and a grid put their least where the error they report under that lens says:
 * 0.046% and 0.044%. Four times over, so the search's own tolerance — a tenth of one misreading —
 * is small beside what it finds: within 1.6%, and it moves linearly. Handed the focal length 8% out as well, the same, because the
 * misreading is under the lens the least was found at, not the one handed in. At twice the
 * resolution, the same: the misreading was half a pixel at first, which halved at 1280 x 960 — the
 * size the page grabs at — and let the chords shape round 7 refused be fitted again (round 8). And
 * on a lens handed in with barrel distortion, the same: the corner's radius is the undistorted one
 * the added k1 acts on, not the pixel's, which read the misreading a quarter too small (round 8).
 * And with the optical centre off the middle of the frame, the same: the corner is the one
 * furthest from it, and every other lens here has its centre in the middle, where any corner is.
 * And under tangential distortion, the same: the furthest corner is the one whose direction is
 * furthest out, which is not always the one furthest in pixels (round 9).
 */
TEST_F(Refine, TheModelErrorIsHowFarALensMisreadMovesTheLeast) {
  const NoisyShapes shapes;
  Intrinsics doubled = TrueLens();
  doubled.fx *= 2.0;
  doubled.fy *= 2.0;
  doubled.cx *= 2.0;
  doubled.cy *= 2.0;
  doubled.width *= 2;
  doubled.height *= 2;
  Intrinsics barrel = TrueLens();
  barrel.k1 = -0.1;
  Intrinsics offCentre = TrueLens();
  // Nearer the far corner than the near one, so the corner furthest from it is (0, 0): with the
  // centre toward the top left, the furthest is (w, h), and a rule that always took that passed.
  offCentre.cx = 440.0;
  offCentre.cy = 330.0;
  Intrinsics tangential = TrueLens();
  // Strong enough that the corners' undistorted radii differ by 15% in their cube; at a third of it
  // they differed by 5%, inside the test's own wander, and a rule that took the corner furthest in
  // pixels failed by a hair (round 9).
  tangential.p1 = 0.015;
  tangential.p2 = -0.012;
  for (const auto& [lens, which] : {std::pair{TrueLens(), "640 x 480"}, std::pair{doubled, "1280 x 960"},
                                    std::pair{barrel, "barrel"}, std::pair{offCentre, "off centre"},
                                    std::pair{tangential, "tangential"}}) {
    // The corner furthest from the optical centre, whichever it is.
    double corner = 0.0;
    for (const double x : {0.0, static_cast<double>(lens.width)}) {
      for (const double y : {0.0, static_cast<double>(lens.height)}) {
        const UnprojectedDirection at = Unproject(lens, Pixel{x, y});
        ASSERT_TRUE(at.valid) << which;
        corner = std::max(corner, std::hypot(at.direction.x, at.direction.y) / -at.direction.z);
      }
    }
    // Four thousandths, so the search's own tolerance is a quarter as large beside what it finds.
    constexpr double kTimes = 4.0;
    const double misread = kTimes * 0.001 / (corner * corner * corner);
    for (const double k1 : {lens.k1 - misread, lens.k1 + misread}) {
      Intrinsics distorted = lens;
      distorted.k1 = k1;
      std::vector<PairwiseResult> grid;
      for (int row = 0; row < 2; ++row) {
        for (int column = 0; column < 3; ++column) {
          grid.push_back(Matched(row * 4 + column, row * 4 + column + 1, shapes.grid, distorted));
        }
      }
      for (int column = 0; column < 4; ++column) {
        grid.push_back(Matched(column, 4 + column, shapes.grid, distorted));
      }
      for (const double scale : {1.0, 1.08}) {
        for (const auto& [pairs, poses, why] :
             {std::tuple{MatchedRing(shapes.truth, distorted), shapes.truth, "a ring"},
              std::tuple{grid, shapes.grid, "a grid"}}) {
          const Result<GlobalSolution> solved =
              engine_.Refine(pairs, PriorsOut(poses), Scaled(lens, scale));
          ASSERT_TRUE(solved.ok()) << which << ", " << why << ", k1 " << k1 << ": " << solved.status.detail;
          ASSERT_TRUE(solved.value.lensFitted) << which << ", " << why << ", k1 " << k1 << ", scale " << scale;
          const double moved = std::abs(std::log(solved.value.intrinsics.fx / lens.fx));
          EXPECT_NEAR(moved / (kTimes * solved.value.focalModelError), 1.0, 0.03)
              << which << ", " << why << ", k1 " << k1 << ", scale " << scale;
        }
      }
    }
  }
}

/**
 * A fit is taken only where it is surer than the lens it would replace (ADR 0067).
 *
 * Against a guess — the lens a page reports, which says nothing of how sure it is — any fit precise
 * enough to take is surer. Against a lens a device kept from earlier captures it need not be: the
 * weak loops a wide lens lets through, fitted within the two tenths of a percent, left rotations
 * ten times worse than an exactly right lens left alone (ADR 0066, round 9). So a lens says how
 * sure it is, the fit is weighed against that, and a fitted lens carries its own figure out.
 */
TEST_F(Refine, AFitIsTakenOnlyWhereItIsSurerThanTheLensItReplaces) {
  const NoisyShapes shapes;
  const Intrinsics guess = TrueLens();
  ASSERT_TRUE(std::isinf(guess.focalUncertainty)) << "the premise: a lens by default is a guess";
  const std::vector<PairwiseResult> pairs = test::WithNoise(shapes.gridPairs, 0.8, 1);

  const Result<GlobalSolution> overGuess = engine_.Refine(pairs, PriorsOut(shapes.grid), guess);
  ASSERT_TRUE(overGuess.ok()) << overGuess.status.detail;
  ASSERT_TRUE(overGuess.value.lensFitted) << "the premise: over a guess, this grid is fitted";
  const double sure = std::hypot(overGuess.value.focalSpread, overGuess.value.focalModelError);
  EXPECT_EQ(overGuess.value.intrinsics.focalUncertainty, sure);

  // Handed the same focal length as a lens kept surer than the fit, it stands, every field of it.
  Intrinsics kept = guess;
  kept.focalUncertainty = 0.9 * sure;
  kept.estimated = true;
  const Result<GlobalSolution> overKept = engine_.Refine(pairs, PriorsOut(shapes.grid), kept);
  ASSERT_TRUE(overKept.ok()) << overKept.status.detail;
  EXPECT_FALSE(overKept.value.lensFitted);
  EXPECT_EQ(overKept.value.intrinsics.fx, kept.fx);
  EXPECT_EQ(overKept.value.intrinsics.focalUncertainty, kept.focalUncertainty);
  // What the fit would have been is still reported, so a caller keeping the lens can see it.
  EXPECT_EQ(overKept.value.focalSpread, overGuess.value.focalSpread);
  EXPECT_EQ(overKept.value.focalModelError, overGuess.value.focalModelError);

  // And one kept less surely than the fit is replaced by it.
  kept.focalUncertainty = 1.1 * sure;
  const Result<GlobalSolution> overLooser = engine_.Refine(pairs, PriorsOut(shapes.grid), kept);
  ASSERT_TRUE(overLooser.ok()) << overLooser.status.detail;
  EXPECT_TRUE(overLooser.value.lensFitted);
  EXPECT_EQ(overLooser.value.intrinsics.fx, overGuess.value.intrinsics.fx);
  EXPECT_EQ(overLooser.value.intrinsics.focalUncertainty, sure);

  // A lens known exactly is never replaced.
  kept.focalUncertainty = 0.0;
  const Result<GlobalSolution> overExact = engine_.Refine(pairs, PriorsOut(shapes.grid), kept);
  ASSERT_TRUE(overExact.ok()) << overExact.status.detail;
  EXPECT_FALSE(overExact.value.lensFitted);
}

/**
 * The noise and the model error are combined, not judged one at a time.
 *
 * The grid at 1.08 px, in this seed, is precise to 0.197% by its noise and 0.045% by its model error
 * — each within the two tenths of a percent a fit is taken at, and 0.202% combined as independent
 * errors, which is not. Judged by the larger of the two it was fitted (round 8). The next seed reads
 * 0.196% combined and is fitted, so the refusal is the combination's and not the grid's.
 */
TEST_F(Refine, NoiseAndModelErrorEachWithinTheThresholdCanBeOverItTogether) {
  const NoisyShapes shapes;
  const Intrinsics lens = TrueLens();
  const Result<GlobalSolution> over =
      engine_.Refine(test::WithNoise(shapes.gridPairs, 1.08, 1), PriorsOut(shapes.grid), lens);
  ASSERT_TRUE(over.ok()) << over.status.detail;
  ASSERT_LT(over.value.focalSpread, 0.002) << "the premise: the noise alone is within it";
  ASSERT_LT(over.value.focalModelError, 0.002) << "the premise: the model error alone is within it";
  EXPECT_GT(std::hypot(over.value.focalSpread, over.value.focalModelError), 0.002);
  EXPECT_FALSE(over.value.lensFitted);

  const Result<GlobalSolution> under =
      engine_.Refine(test::WithNoise(shapes.gridPairs, 1.08, 2), PriorsOut(shapes.grid), lens);
  ASSERT_TRUE(under.ok()) << under.status.detail;
  EXPECT_LT(std::hypot(under.value.focalSpread, under.value.focalModelError), 0.002);
  EXPECT_TRUE(under.value.lensFitted);
}

/**
 * A lens that gives its own frame's corner no direction cannot be misread there, and is not fitted.
 *
 * Its k1 of -0.3 folds the lens inside the frame: matches nearer the centre still have directions,
 * and their loops a least and a spread, but there is no corner radius to misread the lens at. Not
 * left to the misread lens to fail — which it would, for a corner of no direction reads as no
 * number, and then every trial would count as unscored and the spread go unreported with it.
 */
TEST_F(Refine, ALensThatFoldsInsideItsFrameIsNotMisreadOrFitted) {
  Intrinsics folded = TrueLens();
  folded.k1 = -0.3;
  ASSERT_FALSE(Unproject(folded, Pixel{640.0, 480.0}).valid) << "the premise: the corner folds";
  const std::vector<Quat> truth = Ring();
  const Result<GlobalSolution> solved =
      engine_.Refine(MatchedRing(truth, folded), PriorsOut(truth), folded);
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  EXPECT_FALSE(solved.value.lensFitted);
  EXPECT_TRUE(std::isfinite(solved.value.focalSpread)) << solved.value.focalSpread;
  EXPECT_TRUE(std::isinf(solved.value.focalModelError)) << solved.value.focalModelError;
}

/**
 * Matches that disagree with the pair's own inlier count, or that are not pixels, are refused.
 *
 * The count is the weight and the matches are what it counts, so a pair carrying both says the
 * same thing twice; where they differ one of them is wrong and nothing here can say which.
 */
TEST_F(Refine, MatchesThatAreNotThePairsInliersAreRefused) {
  const std::vector<Quat> truth = Ring();
  const std::vector<PairwiseResult> pairs = MatchedRing(truth, TrueLens());
  const std::vector<FramePrior> priors = PriorsOut(truth);
  ASSERT_TRUE(engine_.Refine(pairs, priors, TrueLens()).ok()) << "the base case must be valid";

  // Both ways, since a check that refused only more matches than inliers passes the other.
  for (const int32_t off : {-1, 1}) {
    std::vector<PairwiseResult> miscounted = pairs;
    miscounted[3].inliers += off;
    const Result<GlobalSolution> counted = engine_.Refine(miscounted, priors, TrueLens());
    EXPECT_EQ(counted.status.code, StatusCode::InvalidArgument) << off;
    EXPECT_NE(counted.status.detail.find("matches"), std::string::npos) << counted.status.detail;
  }

  // Each coordinate on its own, since a check that read some of them passes a match wrong in the
  // others.
  for (float PixelMatch::*field : {&PixelMatch::ax, &PixelMatch::ay, &PixelMatch::bx,
                                   &PixelMatch::by}) {
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity()}) {
      std::vector<PairwiseResult> notPixels = pairs;
      notPixels[3].inlierMatches[2].*field = bad;
      const Result<GlobalSolution> answer = engine_.Refine(notPixels, priors, TrueLens());
      EXPECT_EQ(answer.status.code, StatusCode::InvalidArgument) << bad;
      EXPECT_NE(answer.status.detail.find("matches"), std::string::npos) << answer.status.detail;
    }
  }

  // Nor is a match nobody set: a default `PixelMatch` is not a pixel, rather than the image's
  // corner matched to itself, which is finite, counted, and pulled a fit 3% out in a reviewer's
  // probe.
  std::vector<PairwiseResult> unset = pairs;
  unset[3].inlierMatches[2] = PixelMatch{};
  const Result<GlobalSolution> unsetAnswer = engine_.Refine(unset, priors, TrueLens());
  EXPECT_EQ(unsetAnswer.status.code, StatusCode::InvalidArgument);
  EXPECT_NE(unsetAnswer.status.detail.find("matches"), std::string::npos) << unsetAnswer.status.detail;

  // An unaccepted pair's matches are not read, as nothing else it carries is.
  std::vector<PairwiseResult> unread = pairs;
  unread[3].accepted = false;
  unread[3].inliers -= 1;
  EXPECT_TRUE(engine_.Refine(unread, priors, TrueLens()).ok());
}

/**
 * Frames the solve cannot place take no part in the fit.
 *
 * Two frames with no prior, joined only to each other, are an island the solver leaves unplaced at
 * the identity and out of its edge figures. Their pair was once scored against that identity, and
 * its whole angle — which shrinks as the focal length grows — pulled the fit: a 20-match island
 * moved it to 504.6 of 500, and a full one stopped it (a reviewer's reproduction, round 1).
 */
TEST_F(Refine, AnIslandTheSolveCannotPlaceDoesNotMoveTheFit) {
  std::vector<Quat> truth = Ring();
  truth.push_back(AboutY(5));
  truth.push_back(AboutY(25));
  const Intrinsics lens = TrueLens();
  std::vector<FramePrior> priors = PriorsOut(Ring());
  priors.push_back(NoPrior(kFrames));
  priors.push_back(NoPrior(kFrames + 1));

  PairwiseResult full = Matched(kFrames, kFrames + 1, truth, lens);
  PairwiseResult small = full;
  small.inlierMatches.resize(20);
  small.inliers = 20;
  // And one too thin to refit, which is no reason to pass through: the fewest-matches rule is about
  // the pairs the fit is scored on, and this one is not.
  PairwiseResult thin = full;
  thin.inlierMatches.resize(2);
  thin.inliers = 2;
  for (const PairwiseResult& island : {small, full, thin}) {
    std::vector<PairwiseResult> pairs = MatchedRing(Ring(), lens);
    pairs.push_back(island);
    for (const double scale : {0.92, 1.08}) {
      const Result<GlobalSolution> solved = engine_.Refine(pairs, priors, Scaled(lens, scale));
      ASSERT_TRUE(solved.ok()) << solved.status.detail;
      EXPECT_EQ(solved.value.droppedFrames.size(), 2u);
      EXPECT_TRUE(solved.value.lensFitted) << island.inliers << " at " << scale;
      EXPECT_NEAR(solved.value.intrinsics.fx / lens.fx, 1.0, 3e-5) << island.inliers << " at " << scale;
    }
  }
}

/**
 * A focal length the search cannot score at an end of its range is not an answer.
 *
 * Under a lens that folds, a pixel near the edge of the frame has no direction once the focal length
 * is scaled down far enough. A pair whose matches are all out there lost them at the low end of the
 * bracket, the end's cost went infinite, and infinity passed for a cost that rose: the fit was taken
 * at the scale where that pair's matches ran out, with rotations tens of degrees out (a reviewer's
 * reproduction, round 1). Here the truth is outside the bracket, so the answer is the lens as given.
 *
 * The rows reach that answer two ways, and both are wanted. From -1.5 down, the edge pair keeps
 * fewer than three matches with a direction at the short end, so there is no search at all. At
 * -0.8 and -1.0 it keeps six: the search runs on those six at every scale, and it is the rise test
 * that refuses a truth past its end — which is the path the round-1 defect was on, and which the
 * first three rows never reached (a reviewer's instrumentation, round 2).
 */
TEST_F(Refine, AScaleThatLosesMatchesIsNotAFit) {
  const std::vector<Quat> truth = Ring();
  const Intrinsics lens = TrueLens();
  std::vector<PairwiseResult> pairs = MatchedRing(truth, lens);
  PairwiseResult& edgeOnly = pairs[5];
  std::vector<PixelMatch> far;
  for (const PixelMatch& match : edgeOnly.inlierMatches) {
    if (std::hypot(match.ax - lens.cx, match.ay - lens.cy) > 300.0) far.push_back(match);
  }
  ASSERT_GE(far.size(), 3u);
  edgeOnly.inlierMatches = far;
  edgeOnly.inliers = static_cast<int32_t>(far.size());

  for (const double k3 : {-0.8, -1.0, -1.5, -2.0, -3.0}) {
    Intrinsics initial = Scaled(lens, 1.0 / 0.66);
    initial.k3 = k3;
    // Which of the two ways the row takes, so a row that stopped reaching the search would say so.
    const Intrinsics shortest = Scaled(initial, 0.7);
    size_t kept = 0;
    for (const PixelMatch& match : edgeOnly.inlierMatches) {
      if (Unproject(shortest, Pixel{match.ax, match.ay}).valid &&
          Unproject(shortest, Pixel{match.bx, match.by}).valid) {
        ++kept;
      }
    }
    if (k3 > -1.5) {
      ASSERT_GE(kept, 3u) << k3;
    } else {
      ASSERT_LT(kept, 3u) << k3;
    }
    const Result<GlobalSolution> solved = engine_.Refine(pairs, PriorsOut(truth), initial);
    ASSERT_TRUE(solved.ok()) << k3 << ": " << solved.status.detail;
    EXPECT_FALSE(solved.value.lensFitted) << k3;
    EXPECT_EQ(solved.value.intrinsics.fx, initial.fx) << k3;
    // Not the rotations: passed through, they are the solve of the pairs' own `relativeRotation`s,
    // which `Matched` leaves half a degree out on purpose.
  }
}

/**
 * The two trials the rise test adds are trials like any other: one that cannot be scored means no
 * fit, rather than an infinite cost that passes for one that rose.
 *
 * A radial lens that folds, with the truth just past the short end of the bracket. The least is at
 * 0.7, and the rise test's shorter trial lies half a percent below it — below where the matches were
 * filtered. One match sits just inside the fold at 0.7, so it has a direction at every scale the
 * search tries and none at that one trial. Scored as infinity and not as a failure, that trial would
 * pass for a rise, and the longer one rises anyway, so the bracket's own end came back as the focal
 * length (round 3, where the ends of the bracket were the trials in question).
 */
TEST_F(Refine, ATrialTheRiseTestAddsIsScoredLikeTheRest) {
  const std::vector<Quat> truth = Ring();
  Intrinsics handed = Scaled(TrueLens(), 1.0 / 0.695);
  handed.k3 = -1.0;
  std::vector<PairwiseResult> pairs = MatchedRing(truth, Scaled(handed, 0.695));

  // The fold along the image's horizontal through the centre, at the short end of the bracket.
  const Intrinsics shortest = Scaled(handed, 0.7);
  double inside = 0.0;
  double outside = shortest.cx;
  for (int step = 0; step < 60; ++step) {
    const double middle = (inside + outside) / 2.0;
    (Unproject(shortest, Pixel{shortest.cx - middle, shortest.cy}).valid ? inside : outside) = middle;
  }
  const Pixel stray{shortest.cx - inside * 0.999, shortest.cy};
  // The premise: a direction at the short end and none half a percent below it.
  ASSERT_TRUE(Unproject(shortest, stray).valid);
  ASSERT_FALSE(Unproject(Scaled(handed, 0.7 * std::exp(-0.005)), stray).valid);
  const UnprojectedDirection from = Unproject(shortest, stray);
  const Quat exact = Normalize(Multiply(Conjugate(truth[4]), truth[3]));
  const ProjectedPixel to = Project(shortest, Rotate(exact, from.direction));
  ASSERT_TRUE(to.valid);
  ASSERT_TRUE(Unproject(shortest, to.pixel).valid);
  pairs[3].inlierMatches.push_back(
      PixelMatch{static_cast<float>(stray.x), static_cast<float>(stray.y),
                 static_cast<float>(to.pixel.x), static_cast<float>(to.pixel.y)});
  pairs[3].inliers += 1;

  const Result<GlobalSolution> solved = engine_.Refine(pairs, PriorsOut(truth), handed);
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  EXPECT_FALSE(solved.value.lensFitted);
  EXPECT_EQ(solved.value.intrinsics.fx, handed.fx);
  // A trial not scored reaches no least to report a spread for.
  EXPECT_TRUE(std::isinf(solved.value.focalSpread)) << solved.value.focalSpread;
  EXPECT_TRUE(std::isinf(solved.value.focalModelError)) << solved.value.focalModelError;
}

/**
 * A match that loses its direction at a scale the search tries means the fit is not taken, not only
 * one that loses it at the shortest focal length. Not anywhere in the range: a window narrower than
 * the search's steps, 0.7127 to 0.7155 in a reviewer's probe, goes unseen (round 3).
 *
 * Under tangential distortion the pixels `Unproject` accepts are not a star about the centre, so a
 * match with a direction at the short end can lose it partway along and regain it: one pixel of
 * ninety did, and the golden section settled at the edge of the window where it had none — a lens
 * 3.2% out and a median of 0.31 degrees, reported as fitted (a reviewer's reproduction, round 2).
 * The same ring without that one match is fitted, so it is the match that is refused.
 */
TEST_F(Refine, AMatchThatLosesItsDirectionMidRangeIsNotAFit) {
  const std::vector<Quat> truth = Ring();
  Intrinsics handed = Scaled(TrueLens(), 0.6132);
  handed.k1 = -0.1696;
  handed.k2 = -0.1004;
  handed.k3 = 0.0833;
  handed.p1 = -0.0632;
  handed.p2 = 0.0737;
  const Intrinsics lens = Scaled(handed, 1.05);
  std::vector<PairwiseResult> pairs = MatchedRing(truth, lens);

  const Result<GlobalSolution> clean = engine_.Refine(pairs, PriorsOut(truth), handed);
  ASSERT_TRUE(clean.ok()) << clean.status.detail;
  ASSERT_TRUE(clean.value.lensFitted) << "the ring without the stray match must fit";
  EXPECT_NEAR(clean.value.intrinsics.fx / lens.fx, 1.0, 3e-5);

  // A pixel the lens as handed takes, and so one an engine working under it could keep; its
  // partner is built under the same lens.
  const Pixel stray{165.966, 467.627};
  const UnprojectedDirection from = Unproject(handed, stray);
  ASSERT_TRUE(from.valid);
  const Quat exact = Normalize(Multiply(Conjugate(truth[4]), truth[3]));
  const ProjectedPixel to = Project(handed, Rotate(exact, from.direction));
  ASSERT_TRUE(to.valid);
  // The premise the test rests on: a direction at the short end and none somewhere longer.
  ASSERT_TRUE(Unproject(Scaled(handed, 0.7), stray).valid);
  ASSERT_FALSE(Unproject(Scaled(handed, 1.05), stray).valid);
  ASSERT_FALSE(Unproject(Scaled(handed, 1.3), stray).valid);
  pairs[3].inlierMatches.push_back(
      PixelMatch{static_cast<float>(stray.x), static_cast<float>(stray.y),
                 static_cast<float>(to.pixel.x), static_cast<float>(to.pixel.y)});
  pairs[3].inliers += 1;

  const Result<GlobalSolution> solved = engine_.Refine(pairs, PriorsOut(truth), handed);
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  EXPECT_FALSE(solved.value.lensFitted);
  EXPECT_EQ(solved.value.intrinsics.fx, handed.fx);
  // A trial not scored reaches no least to report a spread for.
  EXPECT_TRUE(std::isinf(solved.value.focalSpread)) << solved.value.focalSpread;
  EXPECT_TRUE(std::isinf(solved.value.focalModelError)) << solved.value.focalModelError;
}

// The lens `AMatchThatLosesItsDirectionMidRangeIsNotAFit` is handed, whose tangential terms make
// the pixels `Unproject` accepts something other than a star about the centre.
Intrinsics Tangential() {
  Intrinsics handed = Scaled(TrueLens(), 0.6132);
  handed.k1 = -0.1696;
  handed.k2 = -0.1004;
  handed.k3 = 0.0833;
  handed.p1 = -0.0632;
  handed.p2 = 0.0737;
  return handed;
}

// A match from `stray` in frame 3 to where its direction lands in frame 4, built under `handed`,
// appended to the ring's pair between them.
void AddStray(std::vector<PairwiseResult>& pairs, const std::vector<Quat>& truth,
              const Intrinsics& handed, const Pixel& stray) {
  const UnprojectedDirection from = Unproject(handed, stray);
  ASSERT_TRUE(from.valid);
  const Quat exact = Normalize(Multiply(Conjugate(truth[4]), truth[3]));
  const ProjectedPixel to = Project(handed, Rotate(exact, from.direction));
  ASSERT_TRUE(to.valid);
  pairs[3].inlierMatches.push_back(
      PixelMatch{static_cast<float>(stray.x), static_cast<float>(stray.y),
                 static_cast<float>(to.pixel.x), static_cast<float>(to.pixel.y)});
  pairs[3].inliers += 1;
}

/**
 * The search's own trials count, not only the two the rise test adds: a match that loses its
 * direction at one scale the golden section tries, far from the least, means no fit.
 *
 * Its third trial is fixed by the bracket whatever the cost, since the first two are and the truth
 * lies between them. This match has no direction in a window around that scale and one everywhere
 * else — at the least and half a percent either side of it among them — so the search's trials are
 * the only thing that sees it (round 4, where the rise test's shorter trial had been refusing the
 * mid-range test's match too, and no test told the two apart).
 */
TEST_F(Refine, ATrialTheSearchMakesFarFromTheLeastCounts) {
  const std::vector<Quat> truth = Ring();
  const Intrinsics handed = Tangential();
  std::vector<PairwiseResult> pairs = MatchedRing(truth, Scaled(handed, 1.05));
  ASSERT_TRUE(engine_.Refine(pairs, PriorsOut(truth), handed).value.lensFitted);

  const double golden = (std::sqrt(5.0) - 1.0) / 2.0;
  const double lo = std::log(0.7);
  const double hi = std::log(1.4);
  const double first = hi - golden * (hi - lo);
  const double third = std::exp(first + golden * (hi - first));
  const Pixel stray{196.0, 426.0};
  const auto directed = [&](double scale) { return Unproject(Scaled(handed, scale), stray).valid; };
  ASSERT_FALSE(directed(third)) << third;
  for (const double scale : {0.7, 1.0, 1.05 * std::exp(-0.005), 1.05, 1.05 * std::exp(0.005), 1.4}) {
    ASSERT_TRUE(directed(scale)) << scale;
  }
  AddStray(pairs, truth, handed, stray);

  const Result<GlobalSolution> solved = engine_.Refine(pairs, PriorsOut(truth), handed);
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  EXPECT_FALSE(solved.value.lensFitted);
  EXPECT_EQ(solved.value.intrinsics.fx, handed.fx);
  // A trial not scored reaches no least to report a spread for.
  EXPECT_TRUE(std::isinf(solved.value.focalSpread)) << solved.value.focalSpread;
  EXPECT_TRUE(std::isinf(solved.value.focalModelError)) << solved.value.focalModelError;
}

/**
 * And the rise test's longer trial counts as its shorter one does: a match that loses its direction
 * just past the least's upper step, with the truth just inside the long end of the bracket, means no
 * fit. A radial lens cannot build this — a longer focal length only brings a pixel nearer the centre
 * — and the tangential one can (round 4).
 */
TEST_F(Refine, TheRiseTestsLongerTrialCountsToo) {
  const std::vector<Quat> truth = Ring();
  const Intrinsics handed = Tangential();
  std::vector<PairwiseResult> pairs = MatchedRing(truth, Scaled(handed, 1.398));
  ASSERT_TRUE(engine_.Refine(pairs, PriorsOut(truth), handed).value.lensFitted);

  const Pixel stray{173.5, 460.0};
  const auto directed = [&](double scale) { return Unproject(Scaled(handed, scale), stray).valid; };
  for (const double scale : {0.7, 1.0, 1.398 * std::exp(-0.005), 1.398, 1.4}) {
    ASSERT_TRUE(directed(scale)) << scale;
  }
  ASSERT_FALSE(directed(1.398 * std::exp(0.005)));
  AddStray(pairs, truth, handed, stray);

  const Result<GlobalSolution> solved = engine_.Refine(pairs, PriorsOut(truth), handed);
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  EXPECT_FALSE(solved.value.lensFitted);
  EXPECT_EQ(solved.value.intrinsics.fx, handed.fx);
  // A trial not scored reaches no least to report a spread for.
  EXPECT_TRUE(std::isinf(solved.value.focalSpread)) << solved.value.focalSpread;
  EXPECT_TRUE(std::isinf(solved.value.focalModelError)) << solved.value.focalModelError;
}

}  // namespace
}  // namespace sphanorama
