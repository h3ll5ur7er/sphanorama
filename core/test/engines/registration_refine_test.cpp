// `IRegistrationEngine::Refine` over pairs and priors built by hand, so every answer is known.
//
// Apart from the engine's other tests because it needs no pixels: the pairs are the rotations an
// exact `EstimatePairwise` would have answered with, and the registered frames' own accuracy is
// `registration_accuracy_test.cpp`'s job.

#include <gtest/gtest.h>

#include <cmath>
#include <numbers>
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
std::vector<Quat> Ring() {
  std::vector<Quat> truth;
  for (int i = 0; i < kFrames; ++i) truth.push_back(AboutY(360.0 * i / kFrames));
  return truth;
}

// What `EstimatePairwise(a, b)` answers for an exact pair: `conjugate(q[b]) * q[a]`, spelled here
// rather than borrowed from the solver so a convention inverted in both would still be caught.
PairwiseResult Pair(int a, int b, const std::vector<Quat>& truth, int32_t inliers = 100) {
  PairwiseResult pair;
  pair.a = Frame(a);
  pair.b = Frame(b);
  pair.relativeRotation = Normalize(Multiply(Conjugate(truth[static_cast<size_t>(b)]),
                                             truth[static_cast<size_t>(a)]));
  pair.inliers = inliers;
  pair.correspondences = 2 * inliers;
  pair.accepted = true;
  return pair;
}

// Every consecutive pair and the one that closes the ring.
std::vector<PairwiseResult> RingPairs(const std::vector<Quat>& truth) {
  std::vector<PairwiseResult> pairs;
  for (int i = 0; i < kFrames; ++i) pairs.push_back(Pair(i, (i + 1) % kFrames, truth));
  return pairs;
}

// Priors each three degrees out about an axis of their own, which is how far a fused phone
// orientation is out when it is working.
std::vector<FramePrior> PriorsOut(const std::vector<Quat>& truth, double degrees = 3.0) {
  std::vector<FramePrior> priors;
  for (size_t i = 0; i < truth.size(); ++i) {
    const double at = static_cast<double>(i);
    const Vec3 axis{std::sin(at), std::cos(at), std::sin(2.0 * at)};
    FramePrior prior;
    prior.frame = Frame(static_cast<int>(i));
    prior.pose.orientation = Normalize(Multiply(truth[i], FromAxisAngle(axis, degrees / kDegPerRad)));
    priors.push_back(prior);
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
 */
TEST_F(Refine, ExactPairsGiveTheTruthFacingWhereThePriorsAgree) {
  const std::vector<Quat> truth = Ring();
  const std::vector<FramePrior> priors = PriorsOut(truth);
  Intrinsics lens;
  lens.fx = 512;
  lens.fy = 510;
  lens.cx = 320;
  lens.cy = 240;
  lens.k1 = -0.1;

  const Result<GlobalSolution> solved = engine_.Refine(RingPairs(truth), priors, lens);
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  const GlobalSolution& solution = solved.value;

  ASSERT_EQ(solution.frames.size(), truth.size());
  ASSERT_EQ(solution.rotations.size(), truth.size());
  for (int i = 0; i < kFrames; ++i) {
    EXPECT_EQ(solution.frames[static_cast<size_t>(i)], Frame(i)) << "not in the priors' order";
  }
  EXPECT_TRUE(solution.converged);
  EXPECT_EQ(solution.edgesUsed, kFrames);
  EXPECT_TRUE(solution.droppedFrames.empty());
  EXPECT_TRUE(solution.priorOnlyFrames.empty());
  EXPECT_TRUE(solution.ambiguousFrames.empty());

  const test::RotationScore shape = test::ScoreRotations(solution.rotations, truth);
  ASSERT_TRUE(shape.valid);
  EXPECT_LT(shape.maxDeg, 0.01) << "the pairs are exact, so only the priors' light pull remains";
  EXPECT_LT(solution.maxEdgeErrorDeg, 0.01);

  const test::GaugeAlignment agreed = test::BestGaugeAlignment(Orientations(priors), truth);
  ASSERT_TRUE(agreed.valid);
  EXPECT_NEAR(SeparationDeg(shape.alignment, agreed.rotation), 0.0, 1e-3);

  // Passed through, every field of it: nothing refines a lens yet.
  EXPECT_EQ(solution.intrinsics.fx, lens.fx);
  EXPECT_EQ(solution.intrinsics.fy, lens.fy);
  EXPECT_EQ(solution.intrinsics.cx, lens.cx);
  EXPECT_EQ(solution.intrinsics.cy, lens.cy);
  EXPECT_EQ(solution.intrinsics.k1, lens.k1);
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

  const Result<GlobalSolution> solved = engine_.Refine(pairs, PriorsOut(truth), Intrinsics{});
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  const test::RotationScore score = test::ScoreRotations(solved.value.rotations, truth);
  ASSERT_TRUE(score.valid);
  EXPECT_LT(score.maxDeg, 0.01);
  EXPECT_NEAR(solved.value.medianEdgeErrorDeg, 0.2, 0.01) << "each pair is left its own 0.2 out";
  EXPECT_NEAR(solved.value.maxEdgeErrorDeg, 0.2, 0.01);

  // Without the closing pair the drift has nowhere to go.
  pairs.pop_back();
  const Result<GlobalSolution> open = engine_.Refine(pairs, PriorsOut(truth), Intrinsics{});
  ASSERT_TRUE(open.ok());
  const test::RotationScore drifted = test::ScoreRotations(open.value.rotations, truth);
  ASSERT_TRUE(drifted.valid);
  EXPECT_GT(drifted.maxDeg, 0.5);
}

/**
 * Pairs count by their inliers, so of two that disagree the better-supported one wins.
 *
 * Two frames and two pairs between them, one claiming 30 degrees on 300 inliers and one claiming 38
 * on 100. Weighed by inliers the answer sits a quarter of the way from 30 to 38; weighed equally it
 * would sit halfway.
 */
TEST_F(Refine, APairCountsForItsInliers) {
  const std::vector<Quat> truth{AboutY(0.0), AboutY(30.0)};
  const std::vector<Quat> other{AboutY(0.0), AboutY(38.0)};
  const std::vector<PairwiseResult> pairs{Pair(0, 1, truth, 300), Pair(0, 1, other, 100)};
  std::vector<FramePrior> priors(2);
  priors[0].frame = Frame(0);
  priors[0].pose.orientation = AboutY(0.0);
  priors[1].frame = Frame(1);
  priors[1].pose.orientation = Quat{0, 0, 0, 0};   // no prior: placed through the pairs alone

  const Result<GlobalSolution> solved = engine_.Refine(pairs, priors, Intrinsics{});
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  ASSERT_EQ(solved.value.rotations.size(), 2u);
  const Quat between = Multiply(Conjugate(solved.value.rotations[0]), solved.value.rotations[1]);
  EXPECT_NEAR(SeparationDeg(between, Quat{}), 32.0, 0.01);
}

/**
 * An unaccepted pair neither moves the answer nor refuses it.
 *
 * `accepted` false is a minority of the correspondences agreeing (ADR 0056); the solve leaves it
 * out. Here one says the two frames are a half turn apart, and one carries no rotation at all —
 * which the solver would refuse the whole input over if it were handed it, at any weight.
 */
TEST_F(Refine, AnUnacceptedPairIsLeftOut) {
  const std::vector<Quat> truth = Ring();
  std::vector<PairwiseResult> pairs = RingPairs(truth);

  PairwiseResult wrong = Pair(0, 6, Ring());
  wrong.relativeRotation = Quat{};
  wrong.accepted = false;
  pairs.push_back(wrong);
  PairwiseResult empty = Pair(3, 9, Ring());
  empty.relativeRotation = Quat{0, 0, 0, 0};
  empty.accepted = false;
  pairs.push_back(empty);

  const Result<GlobalSolution> solved = engine_.Refine(pairs, PriorsOut(truth), Intrinsics{});
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  EXPECT_EQ(solved.value.edgesUsed, kFrames);
  const test::RotationScore score = test::ScoreRotations(solved.value.rotations, truth);
  ASSERT_TRUE(score.valid);
  EXPECT_LT(score.maxDeg, 0.01);
}

/**
 * What the solve could not place, and what rests on no pixel, are named by frame.
 *
 * Frames 0 to 2 are joined by accepted pairs. Frame 3 has a prior and no pair: placed, on its prior
 * alone. Frame 4 has neither a usable prior nor an accepted pair: dropped, and absent from both
 * parallel vectors so neither holds a rotation nobody solved for.
 */
TEST_F(Refine, DroppedAndPriorOnlyFramesAreNamed) {
  const std::vector<Quat> truth{AboutY(0.0), AboutY(30.0), AboutY(60.0), AboutY(90.0),
                                AboutY(120.0)};
  const std::vector<PairwiseResult> pairs{Pair(0, 1, truth), Pair(1, 2, truth)};
  std::vector<FramePrior> priors;
  for (int i = 0; i < 5; ++i) {
    FramePrior prior;
    prior.frame = Frame(i);
    prior.pose.orientation = truth[static_cast<size_t>(i)];
    priors.push_back(prior);
  }
  priors[4].pose.orientation = Quat{0, 0, 0, 0};

  const Result<GlobalSolution> solved = engine_.Refine(pairs, priors, Intrinsics{});
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  const GlobalSolution& solution = solved.value;
  ASSERT_EQ(solution.frames.size(), 4u);
  ASSERT_EQ(solution.rotations.size(), 4u);
  for (int i = 0; i < 4; ++i) {
    EXPECT_EQ(solution.frames[static_cast<size_t>(i)], Frame(i));
    EXPECT_NEAR(SeparationDeg(solution.rotations[static_cast<size_t>(i)], truth[static_cast<size_t>(i)]),
                0.0, kSameRotationDeg);
  }
  ASSERT_EQ(solution.droppedFrames.size(), 1u);
  EXPECT_EQ(solution.droppedFrames[0], Frame(4));
  ASSERT_EQ(solution.priorOnlyFrames.size(), 1u);
  EXPECT_EQ(solution.priorOnlyFrames[0], Frame(3));
  EXPECT_EQ(solution.edgesUsed, 2);
}

/**
 * Priors a half turn apart about where a piece sits name its frames as ambiguous, by frame.
 */
TEST_F(Refine, AmbiguousFramesAreNamed) {
  const std::vector<Quat> truth{AboutY(0.0), AboutY(30.0)};
  std::vector<FramePrior> priors(2);
  priors[0].frame = Frame(0);
  priors[0].pose.orientation = truth[0];
  priors[1].frame = Frame(1);
  priors[1].pose.orientation =
      Normalize(Multiply(FromAxisAngle(Vec3{1, 0, 0}, std::numbers::pi), truth[1]));

  const std::vector<PairwiseResult> pairs{Pair(0, 1, truth)};
  const Result<GlobalSolution> solved = engine_.Refine(pairs, priors, Intrinsics{});
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  ASSERT_EQ(solved.value.ambiguousFrames.size(), 2u);
  EXPECT_EQ(solved.value.ambiguousFrames[0], Frame(0));
  EXPECT_EQ(solved.value.ambiguousFrames[1], Frame(1));
}

/**
 * Every input that is not a problem is refused, with the code that says which kind.
 *
 * Each case is the valid ring with one thing wrong, so what refuses it is that thing.
 */
TEST_F(Refine, InputThatIsNotAProblemIsRefused) {
  const std::vector<Quat> truth = Ring();
  const std::vector<PairwiseResult> pairs = RingPairs(truth);
  const std::vector<FramePrior> priors = PriorsOut(truth);
  ASSERT_TRUE(engine_.Refine(pairs, priors, Intrinsics{}).ok()) << "the base case must be valid";

  const auto refused = [&](const std::vector<PairwiseResult>& p, const std::vector<FramePrior>& q,
                           StatusCode code, const char* what) {
    const Result<GlobalSolution> answer = engine_.Refine(p, q, Intrinsics{});
    EXPECT_FALSE(answer.ok()) << what;
    EXPECT_EQ(answer.status.code, code) << what;
    EXPECT_FALSE(answer.status.detail.empty()) << what;
  };

  refused(pairs, {}, StatusCode::InvalidArgument, "no priors");

  std::vector<FramePrior> repeated = priors;
  repeated[5].frame = repeated[4].frame;
  refused(pairs, repeated, StatusCode::InvalidArgument, "a frame given two priors");

  std::vector<FramePrior> unnamed = priors;
  unnamed[2].frame = FrameId{};
  refused(pairs, unnamed, StatusCode::InvalidArgument, "a prior for no frame");

  std::vector<PairwiseResult> stranger = pairs;
  stranger[3].b = Frame(99);
  refused(stranger, priors, StatusCode::InvalidArgument, "a pair naming a frame with no prior");

  // Unaccepted too: a frame nobody gave a prior is the caller and the capture disagreeing about
  // which frames exist, not a measurement to disbelieve.
  stranger[3].accepted = false;
  refused(stranger, priors, StatusCode::InvalidArgument, "an unaccepted pair naming a stranger");

  std::vector<PairwiseResult> self = pairs;
  self[3].b = self[3].a;
  refused(self, priors, StatusCode::InvalidArgument, "a pair from a frame to itself");

  std::vector<PairwiseResult> notRotation = pairs;
  notRotation[3].relativeRotation = Quat{0, 0, 0, 0};
  refused(notRotation, priors, StatusCode::InvalidArgument, "an accepted pair with no rotation");

  std::vector<PairwiseResult> negative = pairs;
  negative[3].inliers = -1;
  refused(negative, priors, StatusCode::InvalidArgument, "an accepted pair with negative inliers");

  std::vector<FramePrior> unusable = priors;
  for (FramePrior& prior : unusable) prior.pose.orientation = Quat{0, 0, 0, 0};
  refused(pairs, unusable, StatusCode::RegistrationFailed, "no usable prior");
}

}  // namespace
}  // namespace sphanorama
