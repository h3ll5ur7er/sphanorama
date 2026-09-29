#include "support/wrong_priors.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

#include <gtest/gtest.h>

#include "support/rotation_scoring.h"
#include "utilities/quaternion.h"

namespace sphanorama::test {
namespace {

constexpr double kDegPerRad = 180.0 / std::numbers::pi;

// The twelve-frame ring the accuracy harnesses render: thirty degrees apart about y.
std::vector<Quat> ARing() {
  std::vector<Quat> ring;
  for (int i = 0; i < 12; ++i) {
    ring.push_back(FromAxisAngle(Vec3{0, 1, 0}, (30.0 * i) / kDegPerRad));
  }
  return ring;
}

// Every registration figure rests on the priors being wrong: at the truth, the estimator is seeded
// with the answer and scores itself (ADR 0057). One edit here reaches every harness, so the three
// degrees are asserted rather than trusted.
TEST(WrongPriors, APairPriorIsTheTrueStepThreeDegreesOutAboutX) {
  const std::vector<Quat> ring = ARing();
  for (size_t a = 0; a < ring.size(); ++a) {
    const size_t b = (a + 1) % ring.size();
    const Quat step = Multiply(Conjugate(ring[b]), ring[a]);
    const Quat prior = PairPriorThreeDegreesOut(ring[a], ring[b]);
    EXPECT_NEAR(AngleBetween(prior, step) * kDegPerRad, 3.0, 1e-6) << a;
    // About x, which the ring does not turn about, so the step being estimated cannot absorb it.
    const Quat off = Normalize(Multiply(Conjugate(step), prior));
    EXPECT_NEAR(off.y, 0.0, 1e-12) << a;
    EXPECT_NEAR(off.z, 0.0, 1e-12) << a;
    // And the step from a to b rather than back: the reverse is sixty degrees away.
    EXPECT_GT(AngleBetween(prior, Multiply(Conjugate(ring[a]), ring[b])) * kDegPerRad, 50.0) << a;
  }
}

TEST(WrongPriors, EveryFramePriorIsThreeDegreesOutInAWayOfItsOwn) {
  const std::vector<Quat> ring = ARing();
  std::vector<FrameId> ids;
  for (size_t i = 0; i < ring.size(); ++i) ids.push_back(FrameId{100 + i});
  const std::vector<FramePrior> priors = FramePriorsThreeDegreesOut(ids, ring);
  ASSERT_EQ(priors.size(), ring.size());
  std::vector<Quat> orientations;
  for (size_t i = 0; i < priors.size(); ++i) {
    EXPECT_EQ(priors[i].frame, ids[i]) << i;
    EXPECT_EQ(priors[i].pose.confidence, 1.0) << i;
    EXPECT_NEAR(AngleBetween(priors[i].pose.orientation, ring[i]) * kDegPerRad, 3.0, 1e-6) << i;
    orientations.push_back(priors[i].pose.orientation);
  }
  // Not one turn shared by all, which would face the whole ring three degrees wrong: what they
  // agree on together is near the truth, and composing a registered ring relies on it being so.
  const GaugeAlignment agreed = BestGaugeAlignment(orientations, ring);
  ASSERT_TRUE(agreed.valid);
  EXPECT_LT(AngleBetween(agreed.rotation, Quat{}) * kDegPerRad, 1.0);
  // Nor one axis in every camera's own frame, which on a ring averages out of that check and still
  // gets every step between neighbours wrong the same way — a consistent wrong shape. Each about an
  // axis of its own, the steps' errors differ.
  std::vector<Quat> stepErrors;
  for (size_t a = 0; a < ring.size(); ++a) {
    const size_t b = (a + 1) % ring.size();
    const Quat truthStep = Multiply(Conjugate(ring[b]), ring[a]);
    const Quat priorStep = Multiply(Conjugate(orientations[b]), orientations[a]);
    stepErrors.push_back(Multiply(Conjugate(truthStep), priorStep));
  }
  double spread = 0.0;
  for (const Quat& error : stepErrors) {
    spread = std::max(spread, AngleBetween(error, stepErrors.front()) * kDegPerRad);
  }
  EXPECT_GT(spread, 1.0);
}

TEST(WrongPriors, ListsOfTwoLengthsAnswerNoPriors) {
  const std::vector<Quat> ring = ARing();
  const std::vector<FrameId> ids{FrameId{1}, FrameId{2}};
  EXPECT_TRUE(FramePriorsThreeDegreesOut(ids, ring).empty());
}

}  // namespace
}  // namespace sphanorama::test
