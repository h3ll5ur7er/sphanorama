// Orientation maths. The expected output of a fused pose is not knowable in advance, but these
// invariants are — and they are the ones the coverage planner and the registration engine will
// silently violate if this is wrong (docs/00 §0.2).
#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <random>

#include "support/same_rotation.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

using test::kSameRotationDeg;
using test::kSameRotationRad;

constexpr double kDegPerRad = 180.0 / std::numbers::pi;

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
  EXPECT_NEAR(AngleBetween(Yaw(30), Yaw(30)), 0.0, kSameRotationRad);
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
  EXPECT_NEAR(AngleBetween(q, negated), 0.0, kSameRotationRad);
}

TEST(AngleBetween, NeverExceedsPi) {
  for (double degrees = 0; degrees <= 360.0; degrees += 15.0) {
    const double angle = AngleBetween(Yaw(0), Yaw(degrees));
    EXPECT_GE(angle, 0.0);
    EXPECT_LE(angle, std::numbers::pi + 1e-9) << "at " << degrees << " degrees";
  }
}

TEST(AngleBetween, IsFiniteForDegenerateInput) {
  EXPECT_TRUE(std::isfinite(AngleBetween(Quat{0, 0, 0, 0}, Yaw(10))));
}

// An angle that is not a measurement gives the identity, like an axis that is not one.
//
// This file's header promises that *every* function here is total — degenerate input yields the
// identity rather than NaN — and the examples beside that promise are all about the axis, which is
// how the angle came to be unguarded. `sin` and `cos` of a NaN are NaN, so every component came back
// NaN and the promise was false for one of its two arguments.
//
// `IsUsableRotation` would have caught the result downstream, which is why nothing failed. That is
// the difference between a defect and a crash, not between a defect and nothing: the header says
// identity, and a caller that trusts it and skips the check gets four NaNs.
TEST(FromAxisAngle, AnAngleThatIsNotAMeasurementGivesTheIdentity) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  const Vec3 axis{0, 1, 0};

  for (const double degenerate : {nan, infinity, -infinity}) {
    const Quat q = FromAxisAngle(axis, degenerate);
    EXPECT_EQ(q.w, 1.0) << "angle " << degenerate;
    EXPECT_EQ(q.x, 0.0) << "angle " << degenerate;
    EXPECT_EQ(q.y, 0.0) << "angle " << degenerate;
    EXPECT_EQ(q.z, 0.0) << "angle " << degenerate;
    EXPECT_TRUE(IsUsableRotation(q)) << "the identity is a rotation; angle " << degenerate;
  }

  // A finite angle still turns, so the guard is refusing the degenerate case rather than everything.
  const Quat real = FromAxisAngle(axis, 1.0);
  EXPECT_NE(real.w, 1.0);
}

TEST(FromAxisAngle, ProducesAUnitQuaternion) {
  EXPECT_NEAR(Norm(FromAxisAngle(Vec3{0, 0, 1}, 1.2)), 1.0, 1e-12);
}

TEST(FromAxisAngle, ARotationOfZeroIsIdentity) {
  const Quat q = FromAxisAngle(Vec3{0, 1, 0}, 0.0);
  EXPECT_NEAR(AngleBetween(q, Quat{}), 0.0, kSameRotationRad);
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

/**
 * An axis whose components are finite and whose *length* is not.
 *
 * The declaration promises identity for "a degenerate axis — zero, or one whose length is not
 * finite", and only the first half had a test: deleting `!std::isfinite(length)` left the whole
 * suite green. The two halves are not the same guard. A zero axis fails `length > 1e-12` and is
 * caught by the other operand; an overflowing one passes it, divides by an infinity, and returns
 * `{cos(half), 0, 0, 0}`.
 *
 * **What makes that worse than the usual degradation is that it is a rotation.** `Norm` is
 * `|cos(half)|`, which for a 57-degree turn is 0.878 — comfortably past `IsUsableRotation`'s 1e-12,
 * so a caller asking "was this measured?" is told yes, and `Normalize` then answers the identity.
 * A caller that did everything right gets "straight ahead" where it asked for a turn. The
 * implementation comment beside the guard reaches for a half turn as its example, where `cos(half)`
 * is zero and the usability gate catches it; the ordinary angle is the dangerous one.
 *
 * Asserted on the norm rather than on the separation from identity, because the wrong answer
 * *normalises* to the identity — a test comparing rotations cannot tell the two apart, and that is
 * how this survived.
 */
TEST(FromAxisAngle, AnAxisWhoseLengthOverflowsYieldsIdentityRatherThanAShortenedQuaternion) {
  const Quat q = FromAxisAngle(Vec3{1e200, 1e200, 0}, 1.0);
  EXPECT_TRUE(std::isfinite(Norm(q)));
  EXPECT_NEAR(Norm(q), 1.0, 1e-12) << "an axis of infinite length produced a sub-unit quaternion";
  EXPECT_TRUE(IsUsableRotation(q));
}

// A quaternion the gate admits is one `Normalize` can derive a rotation from, at both ends of the
// admissible range. Rounds 9, 10 and 11 on PR #80 each found a caller where the two disagreed under
// `clang -O3 -ffp-contract=fast`: the gate's sum of squares compiled to a fused chain and
// `Normalize`'s to a vectorised multiply and three plain adds, so within an ulp of 1e-12 or of
// `sqrt(DBL_MAX)` an admitted quaternion came back as the identity. `Norm` fuses by hand now, and
// this walks across both thresholds to hold it to that.
TEST(Quaternion, EveryQuaternionTheGateAdmitsNormalizesToItsOwnRotation) {
  std::mt19937_64 rng(0x5eed);
  std::normal_distribution<double> gauss(0.0, 1.0);
  struct End {
    const char* name;
    double threshold;
    int admitted = 0;
    int wrong = 0;
  };
  End ends[] = {{"bottom", 1e-12}, {"top", std::sqrt(std::numeric_limits<double>::max())}};
  constexpr int kPool = 64;
  constexpr int kSteps = 4096;
  for (int i = 0; i < kPool; ++i) {
    const Quat unit = Normalize(Quat{gauss(rng), gauss(rng), gauss(rng), gauss(rng)});
    for (End& end : ends) {
      double scale = end.threshold;
      for (int step = 0; step < kSteps / 2; ++step) scale = std::nextafter(scale, 0.0);
      for (int step = 0; step < kSteps; ++step) {
        const Quat scaled{unit.w * scale, unit.x * scale, unit.y * scale, unit.z * scale};
        if (IsUsableRotation(scaled)) {
          ++end.admitted;
          if (AngleBetween(Normalize(scaled), unit) * 180.0 / std::numbers::pi > kSameRotationDeg) {
            ++end.wrong;
          }
        }
        scale = std::nextafter(scale, std::numeric_limits<double>::infinity());
      }
    }
  }
  for (const End& end : ends) {
    // Each end on its own: the walk has to straddle that threshold, admitting some inputs and not
    // all of them, or it never reached the edge it exists to cross. This is also what catches the
    // walk's own copy of 1e-12 drifting from the gate's.
    EXPECT_GT(end.admitted, 0) << end.name;
    EXPECT_LT(end.admitted, kPool * kSteps) << end.name;
    EXPECT_EQ(end.wrong, 0) << "at the " << end.name << " the gate admitted " << end.wrong
                            << " quaternions Normalize could not use";
  }
}

/**
 * The three exceptions the totality promise names, asserted rather than asserted *about*.
 *
 * `quaternion.h`'s opening paragraph says the functions answering a question about orientations are
 * total, and then names three that are not: `Multiply`, which is algebra and answers the zero
 * quaternion; `Conjugate`, which normalises first and so answers the *identity* — the paragraph's
 * own example of a worse failure; and `Norm`, which propagates NaN and infinity because
 * `IsUsableRotation` is built on exactly that.
 *
 * All three were named in a commit whose argument was that a published claim gets an assertion
 * under it, and none of the three got one. The header is the specification and this is the copy
 * that runs — if the two ever disagree, something here fails rather than a reader believing the
 * wrong one.
 */
TEST(Quaternion, TheTotalityPromiseNamesThreeExceptionsAndTheyBehaveAsNamed) {
  // `Multiply` is algebra: zero in, zero out. `IsUsableRotation` then refuses it, which is the
  // whole reason this is the *safer* of the two possible behaviours.
  const Quat zeroProduct = Multiply(Quat{0, 0, 0, 0}, Quat{});
  EXPECT_EQ(zeroProduct.w, 0.0);
  EXPECT_EQ(zeroProduct.x, 0.0);
  EXPECT_EQ(zeroProduct.y, 0.0);
  EXPECT_EQ(zeroProduct.z, 0.0);
  EXPECT_FALSE(IsUsableRotation(zeroProduct)) << "the zero has to stay detectable downstream";

  // `Conjugate` normalises first, so it answers the identity — and `IsUsableRotation` says yes to
  // it. That is the substitution the header calls the worse failure, and it is pinned here so the
  // carve-out cannot quietly stop being true.
  const Quat conjugated = Conjugate(Quat{0, 0, 0, 0});
  EXPECT_NEAR(Norm(conjugated), 1.0, 1e-12) << "it is the identity, not the zero";
  EXPECT_TRUE(IsUsableRotation(conjugated));
  // `Quat{}` by name — `w == 1` exactly — and not merely some representative of the identity
  // rotation. An `AngleBetween(conjugated, Quat{})` arm stood here captioned "not the zero", and it
  // was the one arm of the three that could not tell: `AngleBetween` normalises both inputs and
  // `Normalize(Quat{0,0,0,0})` *is* `Quat{}`, so the zero, `{-1,0,0,0}` and `{2,0,0,0}` all read as
  // zero degrees from it. The `Norm` arm above is what catches the zero; this one catches a sign.
  EXPECT_EQ(conjugated.w, 1.0);
  EXPECT_EQ(conjugated.x, 0.0);
  EXPECT_EQ(conjugated.y, 0.0);
  EXPECT_EQ(conjugated.z, 0.0);

  // `Norm` propagates. A `Norm` that answered zero for a NaN would break the predicate that catches
  // the NaN, so this is correct rather than tolerated.
  EXPECT_TRUE(std::isnan(Norm(Quat{std::numeric_limits<double>::quiet_NaN(), 0, 0, 0})));
  EXPECT_TRUE(std::isinf(Norm(Quat{0, 1e200, 0, 0})));
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
  EXPECT_NEAR(AngleBetween(Multiply(q, Conjugate(q)), Quat{}), 0.0, kSameRotationRad);
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
  EXPECT_NEAR(AngleBetween(FromAzimuthElevation(0, 0), Quat{}), 0.0, kSameRotationRad);
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

TEST(FromAzimuthElevation, MeetsTheDatasetGeneratorAtNumbersNeitherDerived) {
  // The twin of `test_from_azimuth_elevation_meets_the_core_at_numbers_neither_derived` in
  // `tools/test_synth_dataset.py`. Both assert these same decimals, so the two implementations meet
  // an outside reference rather than each other.
  //
  // Why this exists: `synth_dataset.py` promises in a docstring that its poses name the same
  // directions the coverage planner would, and until now nothing executable checked it. Each side
  // was pinned to hand-derived vectors *separately*, which catches a mistake in one but not a
  // convention both share — and a dataset rendered in the wrong rotation convention would register
  // wrong in exactly the compensating way, and score perfect. That is the failure ADR 0050 exists
  // to prevent, so the promise with the largest consequence should not be the one resting on two
  // files having been read side by side.
  //
  // The numbers are worked out from the right-hand rule, calling neither implementation.
  // `FromAzimuthElevation(az, el)` is a yaw about +Y followed by a pitch about +X *in the yawed
  // frame*, so `Rotate(q, v) = yaw(pitch(v))`. Applied to forward (0, 0, -1):
  //   pitch about +X:  (0, +sin el, -cos el)
  //   then yaw about +Y: (-cos el * sin az, sin el, -cos el * cos az)
  // and applied to the camera's +X axis, which the pitch leaves alone:
  //   (cos az, 0, -sin az)
  //
  // The second row is what makes this more than a restatement of `Direction`: a convention that got
  // the forward axis right and the roll wrong would pass on forward alone. And the pair separates a
  // *reversed* composition order — pitch-then-yaw at (37, -12) differs by 0.0419 in its largest
  // component, seven orders over the tolerance here.
  struct Case {
    double azimuth, elevation;
    Vec3 forward, right;
  };
  const Case cases[] = {
      {37.0, -12.0,
       {-0.5886639210, -0.2079116908, -0.7811834080},
       {0.7986355100, 0.0000000000, -0.6018150232}},
      {120.0, 40.0,
       {-0.6634139482, 0.6427876097, 0.3830222216},
       {-0.5000000000, 0.0000000000, -0.8660254038}},
      {250.0, -63.0,
       {0.4266115225, -0.8910065242, 0.1552738958},
       {-0.3420201433, 0.0000000000, 0.9396926208}},
  };

  for (const Case& c : cases) {
    const Quat q = FromAzimuthElevation(c.azimuth, c.elevation);
    const Vec3 f = Rotate(q, Vec3{0, 0, -1});
    const Vec3 r = Rotate(q, Vec3{1, 0, 0});
    SCOPED_TRACE(::testing::Message() << "azimuth " << c.azimuth << ", elevation " << c.elevation);
    EXPECT_NEAR(f.x, c.forward.x, 1e-9);
    EXPECT_NEAR(f.y, c.forward.y, 1e-9);
    EXPECT_NEAR(f.z, c.forward.z, 1e-9);
    EXPECT_NEAR(r.x, c.right.x, 1e-9);
    EXPECT_NEAR(r.y, c.right.y, 1e-9);
    EXPECT_NEAR(r.z, c.right.z, 1e-9);
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

  // A finite length is not enough: `Dot` squares before it sums, so a component large enough
  // overflows and the length is an infinity that `length > 1e-12` waves through. Dividing by it
  // gives NaN wherever the component was itself infinite, and zero elsewhere — half a vector.
  // The `isfinite` half of the guard is what makes this the origin like every other degenerate
  // input, and it had no test: dropping it left all 554 green, because `AngleBetweenDirections`
  // catches both spellings one call later.
  const double inf = std::numeric_limits<double>::infinity();
  for (const Vec3 unusable : {Vec3{inf, 0, 0}, Vec3{1e300, 1e300, 1e300}, Vec3{-inf, inf, 0}}) {
    const Vec3 answered = Normalize(unusable);
    EXPECT_DOUBLE_EQ(answered.x, 0.0);
    EXPECT_DOUBLE_EQ(answered.y, 0.0);
    EXPECT_DOUBLE_EQ(answered.z, 0.0);
  }
}

TEST(AngleBetweenDirections, IgnoresLengthAndMeasuresTheAngle) {
  EXPECT_NEAR(AngleBetweenDirections(Vec3{5, 0, 0}, Vec3{0, 2, 0}) * kDegPerRad, 90.0, 1e-9);
  EXPECT_NEAR(AngleBetweenDirections(Vec3{1, 0, 0}, Vec3{-1, 0, 0}) * kDegPerRad, 180.0, 1e-9);
  EXPECT_NEAR(AngleBetweenDirections(Vec3{1, 0, 0}, Vec3{1, 0, 0}), 0.0, kSameRotationRad);
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

// Every degenerate *quaternion* comes back through `Direction` as something one of the two guards
// above catches, so no caller has ever seen a NaN angle from one.
//
// **This test cannot fail on either guard alone, and that is worth stating rather than fixing.**
// Delete `AngleBetweenDirections`'s degeneracy check and this stays green, because
// `Normalize(Vec3)`'s own gate catches the same inputs one line earlier; delete that instead and
// this guard catches them. Two independent holders of one guarantee, so no test can name which is
// load-bearing here. `ADegenerateDirectionIsNotAnAngleEvenWhenItIsInfinite` above is the one that
// pins `AngleBetweenDirections`'s guard: deleting it alone fails that test and nothing else.
//
// The *reason* it reaches the guard changed on this branch and this paragraph did not follow.
// It used to be that `Normalize` turned an infinite component into NaN per element, which only
// `!(Dot > 0.5)` could catch. `Normalize(Vec3)` now refuses a non-finite length outright, so the
// same input arrives as the origin — still caught, by the same line, for a different reason.
// `Vec3Maths.NormalizeIsTotal` pins that half; it had nothing before, which is how the stale
// sentence survived.
//
// What this test is for, then, is the *guarantee* rather than a guard: that nothing arriving as a
// quaternion can produce an unusable angle. `CaptureSessionManager::ArmBurst` no longer rests on
// it — it checks both of its rotations first — but `Locate` and the reticle still measure angles
// against directions, and this says what those measurements can be.
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

TEST(IsUsableRotation, RefusesAQuaternionWhoseNormIsNotFinite) {
  // Every component finite and the *norm* infinite, which is the case the finiteness check was
  // written for and cannot see: `Norm` squares before it sums, so anything above about 1.34e154
  // overflows on the way. `inf > 1e-12` is true, so the predicate said yes and `Normalize` then
  // divided by infinity and answered `{0,0,0,0}` — neither the input's rotation nor the identity
  // this header promises as the fallback.
  //
  // What that cost, run end to end by the reviewer who found it: `Quat{0, 1e200, 0, 0}` is a 180°
  // flip about X whose real `Direction` is `(0,0,+1)`, and what came out was `(0,0,-1)` — straight
  // ahead. `OrientationPoseEngine::Integrate` then anchored the pose at **confidence 1.0** on a
  // direction 180 degrees from the sample, after which `ArmBurst`'s confidence guard, the cone
  // check (`offBy` exactly 0) and the dwell all pass.
  //
  // `IsUsableRotation` had no test of its own at all. The nearest one, below, feeds `Quat{1e300,…}`
  // through `Direction` and passes because it asserts only that the *angle* comes back finite.
  for (const Quat overflowing : {Quat{0, 1e200, 0, 0}, Quat{1e300, 1e300, 1e300, 1e300},
                                 Quat{1e155, 0, 0, 0}, Quat{0, 0, -1e200, 0}}) {
    EXPECT_FALSE(IsUsableRotation(overflowing))
        << "a quaternion `Normalize` cannot use was reported usable";
    const Quat normalized = Normalize(overflowing);
    EXPECT_DOUBLE_EQ(normalized.w, 1.0) << "the fallback was not the identity the header promises";
    EXPECT_DOUBLE_EQ(normalized.x, 0.0);
    EXPECT_DOUBLE_EQ(normalized.y, 0.0);
    EXPECT_DOUBLE_EQ(normalized.z, 0.0);
  }
}

TEST(IsUsableRotation, AcceptsTheRotationsAPhoneActuallyProduces) {
  // The other side of it, because a predicate that refuses everything is also wrong and would have
  // passed the test above. A unit quaternion, an unnormalised but honest one, and the identity.
  EXPECT_TRUE(IsUsableRotation(FromAzimuthElevation(37.0, -12.0)));
  EXPECT_TRUE(IsUsableRotation(Quat{2, 0, 0, 0}));
  EXPECT_TRUE(IsUsableRotation(Quat{}));
  EXPECT_FALSE(IsUsableRotation(Quat{0, 0, 0, 0}));
}

TEST(RollBetween, IsZeroForTheSameOrientation) {
  const Quat q = FromAzimuthElevation(37.0, -12.0);
  EXPECT_NEAR(RollBetween(q, q), 0.0, kSameRotationRad);
}

TEST(RollBetween, MeasuresRotationAboutTheViewingAxisAndIsSigned) {
  const Quat target = FromAzimuthElevation(20.0, 5.0);
  const Vec3 axis = Direction(target);
  for (const double degrees : {15.0, -40.0, 90.0}) {
    const Quat rolled = Multiply(FromAxisAngle(axis, degrees / kDegPerRad), target);
    EXPECT_NEAR(RollBetween(rolled, target) * kDegPerRad, degrees, 1e-6) << "at " << degrees;
  }
}

/**
 * What `RollBetween` actually does at large separation, which is not what it used to claim.
 *
 * This test asserted `std::isfinite` and nothing else, under a comment saying roll is undefined at
 * opposite directions and that zero is reported there. Both halves were false and the assertion
 * could not see it: 180.0 is perfectly finite. A test whose only predicate is satisfied by every
 * plausible wrong answer is not a test, and this one guarded the exact input its comment described.
 *
 * Pinned as measured, so the behaviour cannot drift unnoticed and so the declaration's new
 * qualification has something executable under it. These are not assertions that the numbers are
 * *right* — the declaration says at length that they are not — they are assertions that they are
 * what they are until someone fixes the function on purpose.
 */
TEST(RollBetween, IsOnlyMeaningfulWhileTheTwoLookTheSameWay) {
  const Quat target = FromAzimuthElevation(0.0, 0.0);

  // Opposite directions: 180, not the zero the comment here used to promise, and responsive to the
  // target's own roll rather than degenerate.
  const Quat away = FromAzimuthElevation(180.0, 0.0);
  EXPECT_NEAR(RollBetween(away, target) * kDegPerRad, 180.0, 1e-6);

  // Ninety degrees of *azimuth* is where the projection collapses, and the zero lands there. The
  // elevation arm reads zero for a different reason and this said they were the same: at elevation
  // 90 `Dot(flattened, flattened)` is **1**, `flattened` is the honest `(1,0,0)`, and the guard is
  // never reached. Two arms, two causes, one explanation covering both — the move the header
  // confesses to about `Conjugate` a few lines into the same commit that wrote this.
  EXPECT_NEAR(RollBetween(FromAzimuthElevation(90.0, 0.0), target) * kDegPerRad, 0.0, 1e-6);
  EXPECT_NEAR(RollBetween(FromAzimuthElevation(0.0, 90.0), target) * kDegPerRad, 0.0, 1e-6);

  // And the discontinuity, which is the reason the declaration says "roughly the same direction":
  // two degrees of aim either side of ninety, at identical roll, differ by half a turn.
  const double justBelow = RollBetween(FromAzimuthElevation(89.0, 0.0), target) * kDegPerRad;
  const double justAbove = RollBetween(FromAzimuthElevation(91.0, 0.0), target) * kDegPerRad;
  EXPECT_NEAR(justBelow, 0.0, 1e-6);
  EXPECT_NEAR(justAbove, 180.0, 1e-6);

  // Near the target, where every caller asks it, it is well behaved — which is what bounds all of
  // the above and is asserted here rather than assumed. Same construction as
  // `MeasuresRotationAboutTheViewingAxisAndIsSigned`, at a separation instead of at zero.
  const Quat nearby = FromAzimuthElevation(3.0, 0.0);
  const Quat rolled = Multiply(FromAxisAngle(Direction(nearby), 10.0 / kDegPerRad), nearby);
  EXPECT_NEAR(RollBetween(rolled, nearby) * kDegPerRad, 10.0, 1e-6);

  // **The two signed figures the declaration publishes**, which it first published positive. They
  // are negative, and the sign is the whole of what "signed and in (-pi, pi]" promises — so leaving
  // them unasserted is what let the declaration be wrong about them. Rolled about
  // `Direction`, the actual viewing axis, rather than about body +Z, which is its negative and is
  // how the wrong sign was measured in the first place.
  const Quat awayRolled =
      Multiply(FromAxisAngle(Direction(away), 30.0 / kDegPerRad), away);
  EXPECT_NEAR(RollBetween(target, awayRolled) * kDegPerRad, -150.0, 1e-6);

  //
  // **The second of them is inert in its roll and the first is not**, which is worth stating beside
  // them rather than leaving for the next reader to discover. Swept: the azimuth-90 arm reads
  // exactly -90 for *every* roll in (0, 180) and +90 for every roll in (-180, 0), so `15.0` could
  // be `3.0` or `179.0` and nothing moves — only the sign is load-bearing. At 90 degrees of
  // separation the projection leaves exactly the ±Y component whatever the roll, which is the
  // collapse the declaration describes three paragraphs above this figure. Its companion is
  // genuinely responsive: 30 -> -150, 60 -> -120, 120 -> -60, 170 -> -10.
  //
  // Kept rather than replaced, because the figure it pins is one the declaration publishes and the
  // sign is what was wrong about it. But an assertion that does not measure what it appears to
  // measure is a trap for whoever edits it next, so it says so.
  const Quat side = FromAzimuthElevation(90.0, 0.0);
  const Quat sideRolled = Multiply(FromAxisAngle(Direction(side), 15.0 / kDegPerRad), side);
  EXPECT_NEAR(RollBetween(target, sideRolled) * kDegPerRad, -90.0, 1e-6);
  // And the same at a roll eleven times larger, so "the roll is not in the answer" is asserted
  // rather than only stated: two rolls reading the same -90 is the collapse, in executable form.
  // The *magnitude*, precisely — the answer is `sign(sin roll) * 90`, so this arm and the one above
  // are the same assertion (a sign flip fails both, nothing fails one without the other) and what the
  // pair pins is that the roll's size has left the answer entirely.
  const Quat sideRolledFar = Multiply(FromAxisAngle(Direction(side), 170.0 / kDegPerRad), side);
  EXPECT_NEAR(RollBetween(target, sideRolledFar) * kDegPerRad, -90.0, 1e-6)
      << "at ninety degrees of separation the magnitude of the roll is not in the answer";
}

/**
 * The collapse guard, pinned by an input where removing it changes the answer.
 *
 * `IsOnlyMeaningfulWhileTheTwoLookTheSameWay` has an assertion claiming to pin "where the
 * projection actually collapses", and it does not: the whole suite passes with the guard made
 * unreachable. It was derived from the case in hand — a *level* phone at azimuth 90, where the
 * unguarded `atan2` of two zeros is zero and agrees with the guarded answer by luck.
 *
 * The guard is not redundant. When it fires, `flattened` is `Normalize`'s zero-vector fallback, so
 * both `atan2` operands are signed zeros and the sign of `x` decides between 0 and half a turn.
 * `x` is `-0.0` exactly when all three components of `here` are negative — over 2,000,000
 * constructed collapses that predicate agrees with "the guard changed the answer" 100.000% of the
 * time, at 12.548% against an analytic one in eight. Random orientations never reach it at all:
 * 4,000,000 random pairs produce zero collapses, so the input has to be built.
 *
 * What was missing was a *rolled* current. Both arms below read zero as committed and ±180 with the
 * guard unreachable, which is the difference the two arms in the other test cannot see.
 */
TEST(RollBetween, TheCollapseGuardDecidesBetweenZeroAndAHalfTurn) {
  const Quat target = FromAzimuthElevation(0.0, 0.0);

  const Quat side = FromAzimuthElevation(90.0, 0.0);
  const Quat rolledSide = Multiply(FromAxisAngle(Direction(side), 45.0 / kDegPerRad), side);
  EXPECT_NEAR(RollBetween(rolledSide, target) * kDegPerRad, 0.0, 1e-6)
      << "without the collapse guard this is -180";

  const Quat other = FromAzimuthElevation(-90.0, 0.0);
  const Quat rolledOther = Multiply(FromAxisAngle(Direction(other), 180.0 / kDegPerRad), other);
  EXPECT_NEAR(RollBetween(rolledOther, target) * kDegPerRad, 0.0, 1e-6)
      << "without the collapse guard this is +180";
}

}  // namespace
// No pose is spelled with `confidence` zero, and at zero the orientation is not read: an unset pose
// and a gyroscope's unanchored one are both well-formed. Everything else outside [0, 1], and an
// orientation that is not a rotation where the confidence claims one, is a defect with a reason.
TEST(PoseSampleDefect, ZeroConfidenceIsNoPoseWhateverTheOrientationSays) {
  EXPECT_FALSE(PoseSampleDefect(PoseSample{}).has_value());
  PoseSample unread;
  unread.orientation = Quat{0, 0, 0, 0};
  EXPECT_FALSE(PoseSampleDefect(unread).has_value());
}

TEST(PoseSampleDefect, EveryConfidenceThePoseEngineWritesIsWellFormed) {
  for (const double confidence : {0.5, 1.0}) {
    PoseSample pose;
    pose.confidence = confidence;
    EXPECT_FALSE(PoseSampleDefect(pose).has_value()) << confidence;
  }
}

TEST(PoseSampleDefect, AConfidenceOutsideTheUnitIntervalIsADefect) {
  for (const double confidence : {-0.25, 1.5, std::numeric_limits<double>::quiet_NaN(),
                                  std::numeric_limits<double>::infinity()}) {
    PoseSample pose;
    pose.confidence = confidence;
    const std::optional<std::string_view> defect = PoseSampleDefect(pose);
    ASSERT_TRUE(defect.has_value()) << confidence;
    EXPECT_NE(defect->find("confidence outside"), std::string_view::npos) << *defect;
  }
}

TEST(PoseSampleDefect, AClaimedOrientationThatIsNotARotationIsADefect) {
  for (const Quat& orientation :
       {Quat{0, 0, 0, 0}, Quat{std::numeric_limits<double>::quiet_NaN(), 0, 0, 0}}) {
    PoseSample pose;
    pose.orientation = orientation;
    pose.confidence = 0.5;
    const std::optional<std::string_view> defect = PoseSampleDefect(pose);
    ASSERT_TRUE(defect.has_value());
    EXPECT_NE(defect->find("an orientation that is not a rotation"), std::string_view::npos)
        << *defect;
  }
}

}  // namespace sphanorama
