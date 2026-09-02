// Orientation maths. The expected output of a fused pose is not knowable in advance, but these
// invariants are — and they are the ones the coverage planner and the registration engine will
// silently violate if this is wrong (docs/00 §0.2).
#include <gtest/gtest.h>

#include <cmath>

#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegPerRad = 180.0 / kPi;

Quat Yaw(double degrees) {
  return FromAxisAngle(Vec3{0, 1, 0}, degrees / kDegPerRad);
}

TEST(Normalize, MakesAUnitQuaternion) {
  const Quat q = Normalize(Quat{2, 0, 0, 0});
  EXPECT_NEAR(Norm(q), 1.0, 1e-12);
}

TEST(Normalize, LeavesADegenerateQuaternionAsIdentityRatherThanNaN) {
  // A zero quaternion arrives from uninitialised sensor data more often than it should. Dividing
  // by its norm would poison every downstream rotation with NaN, and NaN does not fail loudly.
  const Quat q = Normalize(Quat{0, 0, 0, 0});
  EXPECT_DOUBLE_EQ(q.w, 1.0);
  EXPECT_NEAR(Norm(q), 1.0, 1e-12);
}

TEST(AngleBetween, IsZeroForIdenticalOrientations) {
  EXPECT_NEAR(AngleBetween(Yaw(30), Yaw(30)), 0.0, 1e-9);
}

TEST(AngleBetween, MeasuresTheRotationSeparatingTwoOrientations) {
  EXPECT_NEAR(AngleBetween(Yaw(0), Yaw(45)) * kDegPerRad, 45.0, 1e-6);
  EXPECT_NEAR(AngleBetween(Yaw(10), Yaw(100)) * kDegPerRad, 90.0, 1e-6);
}

TEST(AngleBetween, IsSymmetric) {
  EXPECT_NEAR(AngleBetween(Yaw(20), Yaw(95)), AngleBetween(Yaw(95), Yaw(20)), 1e-12);
}

TEST(AngleBetween, TreatsQAndMinusQAsTheSameOrientation) {
  // A quaternion and its negation are the same rotation. A planner that missed this would report
  // a cell as 180 degrees away the moment the sensor's sign flipped, and the reticle would jump.
  const Quat q = Yaw(37);
  const Quat negated{-q.w, -q.x, -q.y, -q.z};
  EXPECT_NEAR(AngleBetween(q, negated), 0.0, 1e-9);
}

TEST(AngleBetween, NeverExceedsPi) {
  for (double degrees = 0; degrees <= 360.0; degrees += 15.0) {
    const double angle = AngleBetween(Yaw(0), Yaw(degrees));
    EXPECT_GE(angle, 0.0);
    EXPECT_LE(angle, kPi + 1e-9) << "at " << degrees << " degrees";
  }
}

TEST(AngleBetween, IsFiniteForDegenerateInput) {
  EXPECT_TRUE(std::isfinite(AngleBetween(Quat{0, 0, 0, 0}, Yaw(10))));
}

TEST(FromAxisAngle, ProducesAUnitQuaternion) {
  EXPECT_NEAR(Norm(FromAxisAngle(Vec3{0, 0, 1}, 1.2)), 1.0, 1e-12);
}

TEST(FromAxisAngle, ARotationOfZeroIsIdentity) {
  const Quat q = FromAxisAngle(Vec3{0, 1, 0}, 0.0);
  EXPECT_NEAR(AngleBetween(q, Quat{}), 0.0, 1e-12);
}

TEST(FromAxisAngle, IgnoresAxisLength) {
  // Callers pass unnormalised axes constantly; scaling the axis must not scale the rotation.
  const double a = AngleBetween(Quat{}, FromAxisAngle(Vec3{0, 1, 0}, 0.7));
  const double b = AngleBetween(Quat{}, FromAxisAngle(Vec3{0, 5, 0}, 0.7));
  EXPECT_NEAR(a, b, 1e-12);
}

TEST(FromAxisAngle, ADegenerateAxisYieldsIdentityRatherThanNaN) {
  const Quat q = FromAxisAngle(Vec3{0, 0, 0}, 1.0);
  EXPECT_TRUE(std::isfinite(q.w));
  EXPECT_NEAR(Norm(q), 1.0, 1e-12);
}

TEST(Direction, PointsForwardForIdentity) {
  const Vec3 forward = Direction(Quat{});
  EXPECT_NEAR(forward.x, 0.0, 1e-12);
  EXPECT_NEAR(forward.y, 0.0, 1e-12);
  EXPECT_NEAR(forward.z, -1.0, 1e-12);
}

TEST(Direction, TurnsWithTheOrientation) {
  // Rotating the forward vector (-Z) about +Y by theta gives (-sin theta, 0, -cos theta), so a
  // quarter turn lands the view on the X axis with the sign following the rotation direction.
  const Vec3 quarter = Direction(Yaw(90));
  EXPECT_NEAR(quarter.x, -1.0, 1e-6);
  EXPECT_NEAR(quarter.z, 0.0, 1e-6);

  const Vec3 opposite = Direction(Yaw(-90));
  EXPECT_NEAR(opposite.x, 1.0, 1e-6);
  EXPECT_NEAR(opposite.z, 0.0, 1e-6);
}

TEST(Direction, AHalfTurnLooksBackwards) {
  const Vec3 behind = Direction(Yaw(180));
  EXPECT_NEAR(behind.z, 1.0, 1e-6);
}

TEST(Direction, IsAlwaysUnitLength) {
  for (double degrees = 0; degrees < 360.0; degrees += 30.0) {
    const Vec3 d = Direction(Yaw(degrees));
    EXPECT_NEAR(std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z), 1.0, 1e-9);
  }
}

}  // namespace
}  // namespace sphanorama
