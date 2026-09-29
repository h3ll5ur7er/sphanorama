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
  // Wrong relative to each other, not only together: a turn shared by every frame leaves the
  // priors' shape exact, and the gauge-free score is how far their shape is from the ring's. Three
  // degrees each, so about three; zero is the frame-level form of handing the solve the truth.
  EXPECT_GT(ScoreRotations(orientations, ring).medianDeg, 2.0);
  // Each about an axis of its own, taken in its camera's frame and compared as lines, so no two
  // frames share one either way round. The closest pair of the helper's axes is 11.96 degrees.
  std::vector<Vec3> axes;
  for (size_t i = 0; i < priors.size(); ++i) {
    const Quat off = Multiply(Conjugate(ring[i]), orientations[i]);
    axes.push_back(Normalize(Vec3{off.x, off.y, off.z}));
  }
  double closest = 180.0;
  for (size_t a = 0; a < axes.size(); ++a) {
    for (size_t b = a + 1; b < axes.size(); ++b) {
      const double along = std::abs(Dot(axes[a], axes[b]));
      closest = std::min(closest, std::acos(std::min(along, 1.0)) * kDegPerRad);
    }
  }
  EXPECT_GT(closest, 10.0);
}

TEST(WrongPriors, ListsOfTwoLengthsAnswerNoPriors) {
  const std::vector<Quat> ring = ARing();
  EXPECT_TRUE(FramePriorsThreeDegreesOut({FrameId{1}, FrameId{2}}, ring).empty());
  std::vector<FrameId> more;
  for (size_t i = 0; i <= ring.size(); ++i) more.push_back(FrameId{1 + i});
  EXPECT_TRUE(FramePriorsThreeDegreesOut(more, ring).empty());
  EXPECT_TRUE(FramePriorsThreeDegreesOut(more, {}).empty());
}

}  // namespace
}  // namespace sphanorama::test
