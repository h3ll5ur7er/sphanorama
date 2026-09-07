// Orientation maths. The expected output of a fused pose is not knowable in advance, but these
// invariants are — and they are the ones the coverage planner and the registration engine will
// silently violate if this is wrong (docs/00 §0.2).
#include <gtest/gtest.h>

#include <cmath>
#include <limits>

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

TEST(Conjugate, UndoesARotation) {
  const Quat q = FromAxisAngle(Vec3{0.3, 1.0, -0.2}, 0.8);
  EXPECT_NEAR(AngleBetween(Multiply(q, Conjugate(q)), Quat{}), 0.0, 1e-9);
}

TEST(Rotate, LeavesAVectorAloneUnderIdentity) {
  const Vec3 v = Rotate(Quat{}, Vec3{1, 2, 3});
  EXPECT_NEAR(v.x, 1.0, 1e-12);
  EXPECT_NEAR(v.y, 2.0, 1e-12);
  EXPECT_NEAR(v.z, 3.0, 1e-12);
}

TEST(Rotate, PreservesLength) {
  const Quat q = FromAxisAngle(Vec3{1, 2, 3}, 1.1);
  const Vec3 v = Rotate(q, Vec3{0, 0, -1});
  EXPECT_NEAR(std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z), 1.0, 1e-9);
}

TEST(Rotate, AgreesWithDirection) {
  // Direction(q) is defined as the forward axis rotated by q; if these two ever disagree the
  // planner and the tests that check it would be measuring different geometry.
  const Quat q = FromAzimuthElevation(37.0, -12.0);
  const Vec3 byRotate = Rotate(q, Vec3{0, 0, -1});
  const Vec3 byDirection = Direction(q);
  EXPECT_NEAR(byRotate.x, byDirection.x, 1e-12);
  EXPECT_NEAR(byRotate.y, byDirection.y, 1e-12);
  EXPECT_NEAR(byRotate.z, byDirection.z, 1e-12);
}

TEST(Rotate, RoundTripsThroughTheConjugate) {
  const Quat q = FromAzimuthElevation(120.0, 40.0);
  const Vec3 original{0.2, -0.5, 0.84};
  const Vec3 back = Rotate(Conjugate(q), Rotate(q, original));
  EXPECT_NEAR(back.x, original.x, 1e-9);
  EXPECT_NEAR(back.y, original.y, 1e-9);
  EXPECT_NEAR(back.z, original.z, 1e-9);
}

TEST(FromAzimuthElevation, PointsForwardAtTheOrigin) {
  EXPECT_NEAR(AngleBetween(FromAzimuthElevation(0, 0), Quat{}), 0.0, 1e-12);
}

TEST(FromAzimuthElevation, ElevationLooksUpAndDown) {
  EXPECT_NEAR(Direction(FromAzimuthElevation(0, 90)).y, 1.0, 1e-6);
  EXPECT_NEAR(Direction(FromAzimuthElevation(0, -90)).y, -1.0, 1e-6);
}

TEST(FromAzimuthElevation, AzimuthSweepsTheHorizon) {
  for (double azimuth = 0; azimuth < 360.0; azimuth += 45.0) {
    EXPECT_NEAR(Direction(FromAzimuthElevation(azimuth, 0)).y, 0.0, 1e-9)
        << "at azimuth " << azimuth;
  }
}

TEST(FromAzimuthElevation, IsPeriodicInAzimuth) {
  EXPECT_NEAR(AngleBetween(FromAzimuthElevation(10, 20), FromAzimuthElevation(370, 20)),
              0.0, 1e-9);
}


TEST(Vec3Maths, DotAndCrossFollowTheRightHandRule) {
  const Vec3 x{1, 0, 0};
  const Vec3 y{0, 1, 0};
  EXPECT_DOUBLE_EQ(Dot(x, y), 0.0);
  EXPECT_DOUBLE_EQ(Dot(x, x), 1.0);
  const Vec3 z = Cross(x, y);
  EXPECT_NEAR(z.x, 0.0, 1e-12);
  EXPECT_NEAR(z.y, 0.0, 1e-12);
  EXPECT_NEAR(z.z, 1.0, 1e-12);
}

TEST(Vec3Maths, NormalizeIsTotal) {
  // Same rule as the quaternion side: a degenerate input yields a zero vector a caller can test
  // for, never a NaN that propagates silently through a whole session.
  const Vec3 zero = Normalize(Vec3{0, 0, 0});
  EXPECT_DOUBLE_EQ(zero.x, 0.0);
  EXPECT_DOUBLE_EQ(zero.y, 0.0);
  EXPECT_DOUBLE_EQ(zero.z, 0.0);
  const Vec3 unit = Normalize(Vec3{0, 3, 4});
  EXPECT_NEAR(std::sqrt(Dot(unit, unit)), 1.0, 1e-12);
}

TEST(AngleBetweenDirections, IgnoresLengthAndMeasuresTheAngle) {
  EXPECT_NEAR(AngleBetweenDirections(Vec3{5, 0, 0}, Vec3{0, 2, 0}) * kDegPerRad, 90.0, 1e-9);
  EXPECT_NEAR(AngleBetweenDirections(Vec3{1, 0, 0}, Vec3{-1, 0, 0}) * kDegPerRad, 180.0, 1e-9);
  EXPECT_NEAR(AngleBetweenDirections(Vec3{1, 0, 0}, Vec3{1, 0, 0}), 0.0, 1e-9);
}

TEST(AngleBetweenDirections, ADegenerateDirectionIsNotAnAngleEvenWhenItIsInfinite) {
  // The function's own comment says "a degenerate direction is not an angle" and returns 0.0 for
  // one. It got that right for a zero vector and for NaN — both normalise to zero, and the
  // `Dot(x, x) < 0.5` test catches them — and wrong for an infinite component, which normalises to
  // `inf/inf` = NaN *per element*: `Dot(x, x)` is then NaN, `NaN < 0.5` is false, and the
  // degeneracy check hands NaN through to `acos`.
  //
  // It matters because the answer is an angular error, and every caller compares it against a
  // threshold. A NaN loses every comparison, so `angle > cone` reads as "inside the cone" and
  // `angle <= cone` reads as "outside" — the same number arriving at two callers as two different
  // answers, which is the shape of defect this branch has now closed three times.
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const Vec3 degenerate : {Vec3{inf, 0, 0}, Vec3{-inf, 0, 0}, Vec3{nan, 0, 0}, Vec3{0, 0, 0},
                                Vec3{inf, inf, inf}}) {
    EXPECT_DOUBLE_EQ(AngleBetweenDirections(degenerate, Vec3{1, 0, 0}), 0.0);
    EXPECT_DOUBLE_EQ(AngleBetweenDirections(Vec3{1, 0, 0}, degenerate), 0.0);
  }
}

// Every degenerate *quaternion* already reaches that guard as something it catches, which is why
// no caller going through `Direction` has ever seen a NaN angle. Pinned rather than assumed: it is
// the reason `CaptureSessionManager::ArmBurst` can compare `offBy` against a cone with an ordinary
// `>` instead of a NaN-proof spelling, and that reasoning is only as good as this test.
TEST(AngleBetweenDirections, EveryDegenerateQuaternionStillMeasuresAFiniteAngle) {
  const double inf = std::numeric_limits<double>::infinity();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  for (const Quat broken : {Quat{inf, 0, 0, 0}, Quat{nan, 1, 0, 0}, Quat{0, 0, 0, 0},
                            Quat{1e300, 1e300, 1e300, 1e300}, Quat{-inf, inf, nan, 0}}) {
    const double angle = AngleBetweenDirections(Direction(broken), Vec3{1, 0, 0});
    EXPECT_TRUE(std::isfinite(angle)) << "a degenerate quaternion produced an unusable angle";
    EXPECT_GE(angle, 0.0);
    EXPECT_LE(angle, 3.15);
  }
}

TEST(RollBetween, IsZeroForTheSameOrientation) {
  const Quat q = FromAzimuthElevation(37.0, -12.0);
  EXPECT_NEAR(RollBetween(q, q), 0.0, 1e-12);
}

TEST(RollBetween, MeasuresRotationAboutTheViewingAxisAndIsSigned) {
  const Quat target = FromAzimuthElevation(20.0, 5.0);
  const Vec3 axis = Direction(target);
  for (const double degrees : {15.0, -40.0, 90.0}) {
    const Quat rolled = Multiply(FromAxisAngle(axis, degrees / kDegPerRad), target);
    EXPECT_NEAR(RollBetween(rolled, target) * kDegPerRad, degrees, 1e-6) << "at " << degrees;
  }
}

TEST(RollBetween, SaysNothingRatherThanNonsenseWhenLookingTheOppositeWay) {
  // Roll about an axis is undefined once the two frames point opposite ways; reporting zero is
  // the honest answer, and it keeps the number out of NaN territory.
  const Quat target = FromAzimuthElevation(0.0, 0.0);
  const Quat away = FromAzimuthElevation(180.0, 0.0);
  EXPECT_TRUE(std::isfinite(RollBetween(away, target)));
}

}  // namespace
}  // namespace sphanorama
