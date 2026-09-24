#include "utilities/quaternion_average.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include "support/same_rotation.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

using test::kSameRotationDeg;
using test::kSameRotationRad;

constexpr double kDegPerRad = 180.0 / std::numbers::pi;

Quat AboutY(double degrees) { return FromAxisAngle(Vec3{0, 1, 0}, degrees / kDegPerRad); }

double SeparationDeg(const Quat& a, const Quat& b) { return AngleBetween(a, b) * kDegPerRad; }

/**
 * Where a weighted average of rotations *about one axis* lands, in closed form.
 *
 * About a common axis the problem collapses to two dimensions: a rotation of `a` degrees is the
 * quaternion `(cos(a/2), sin(a/2) * axis)`, its outer product's traceless part is built from
 * `cos(a)` and `sin(a)`, and the principal eigenvector of the weighted sum therefore sits at
 * `atan2(sum w sin a, sum w cos a)`. So an expected value is knowable in advance here rather than
 * merely bounded, which is what lets the tests below assert a number.
 *
 * Derived rather than measured from the implementation: a fixture that took its expectation from
 * the code under test would certify whatever that code does.
 */
double ExpectedAngleDeg(const std::vector<double>& degrees, const std::vector<double>& weights) {
  double s = 0;
  double c = 0;
  for (size_t i = 0; i < degrees.size(); ++i) {
    s += weights[i] * std::sin(degrees[i] / kDegPerRad);
    c += weights[i] * std::cos(degrees[i] / kDegPerRad);
  }
  return std::atan2(s, c) * kDegPerRad;
}

/**
 * One rotation averaged is that rotation.
 *
 * The weakest statement the function makes, and the one every other test here is measured against:
 * with a single input the outer-product sum is rank one, its principal eigenvector is the input,
 * and anything else means the eigensolver is not solving.
 */
TEST(AverageQuaternions, OneRotationIsItsOwnAverage) {
  const std::vector<Quat> only{AboutY(37.0)};
  const QuaternionAverage average = AverageQuaternions(only, {});
  ASSERT_TRUE(average.valid);
  EXPECT_NEAR(SeparationDeg(average.rotation, only[0]), 0.0, kSameRotationDeg);
}

/**
 * The average of a rotation and itself negated is that rotation, not the identity.
 *
 * `q` and `-q` are the same orientation, and the component-wise mean of the two is the zero
 * quaternion — which `Normalize` answers with the identity, a perfectly ordinary rotation pointing
 * somewhere else entirely. Squaring is what makes the hemisphere question disappear rather than
 * needing handling: the outer product is invariant under `q -> -q`, so both inputs contribute the
 * same matrix.
 *
 * This is the case that separates Markley's average from the arithmetic mean reached for first, and
 * it is reachable in life — nothing upstream of this normalises a measured quaternion's sign.
 */
TEST(AverageQuaternions, ASignFlippedCopyIsTheSameOrientationAndNotACancellingOne) {
  const Quat turn = AboutY(37.0);
  const std::vector<Quat> both{turn, Quat{-turn.w, -turn.x, -turn.y, -turn.z}};
  const QuaternionAverage average = AverageQuaternions(both, {});
  ASSERT_TRUE(average.valid);
  EXPECT_NEAR(SeparationDeg(average.rotation, turn), 0.0, kSameRotationDeg);
}

/**
 * Two rotations about one axis average to the angle halfway between them.
 *
 * The simplest case of `ExtractAngleDeg`'s closed form, spelled out with its own literal so that a
 * mistake in the helper and a matching mistake in the implementation cannot agree with each other.
 */
TEST(AverageQuaternions, TwoTurnsAboutOneAxisAverageToTheAngleBetweenThem) {
  const std::vector<Quat> pair{AboutY(10.0), AboutY(50.0)};
  ASSERT_NEAR(ExpectedAngleDeg({10.0, 50.0}, {1.0, 1.0}), 30.0, 1e-9);

  const QuaternionAverage average = AverageQuaternions(pair, {});
  ASSERT_TRUE(average.valid);
  EXPECT_NEAR(SeparationDeg(average.rotation, AboutY(30.0)), 0.0, kSameRotationDeg);
}

/**
 * A weight moves the answer, and moves it the amount the closed form says.
 *
 * The point of the weights is that a caller with unequal evidence can say so, and the assertion has
 * to be a value rather than an inequality: "closer to the heavier one" is satisfied by a function
 * that ignores the weights entirely and answers the heavier input.
 *
 * Three to one across a forty-degree gap lands 9.686 degrees from the heavier rotation. Both
 * distances are asserted, so a sign error in the weighting cannot be absorbed by symmetry — and the
 * closed form's own number is pinned first, so a change to the helper fails here rather than moving
 * the expectation along with the answer.
 */
TEST(AverageQuaternions, AWeightMovesTheAnswerTowardTheHeavierRotationByAMeasuredAmount) {
  const std::vector<Quat> pair{AboutY(10.0), AboutY(50.0)};
  const std::vector<double> weights{3.0, 1.0};

  const double expected = ExpectedAngleDeg({10.0, 50.0}, weights);
  ASSERT_NEAR(expected, 19.685895, 1e-6) << "the closed form these figures come from has moved";

  const QuaternionAverage average = AverageQuaternions(pair, weights);
  ASSERT_TRUE(average.valid);
  EXPECT_NEAR(SeparationDeg(average.rotation, pair[0]), expected - 10.0, 1e-9);
  EXPECT_NEAR(SeparationDeg(average.rotation, pair[1]), 50.0 - expected, 1e-9);
}

/**
 * A zero weight removes a rotation from the average without removing it from the input.
 *
 * This is the property the rotation solver needs for an unaccepted pair (ADR 0056): the caller holds
 * a rotation the pixels offered and does not want it counted, and rebuilding the input without it
 * would mean rebuilding the index space every parallel array is addressed by.
 */
TEST(AverageQuaternions, AZeroWeightRemovesARotationFromTheAverage) {
  const std::vector<Quat> three{AboutY(10.0), AboutY(50.0), AboutY(170.0)};

  const QuaternionAverage without = AverageQuaternions(three, std::vector<double>{1, 1, 0});
  ASSERT_TRUE(without.valid);
  EXPECT_NEAR(SeparationDeg(without.rotation, AboutY(30.0)), 0.0, kSameRotationDeg);

  // And it is the zero doing the removing rather than the ordering: counted, the third input moves
  // the answer to 60 degrees, which is where the closed form puts three equal weights.
  const QuaternionAverage with = AverageQuaternions(three, std::vector<double>{1, 1, 1});
  ASSERT_TRUE(with.valid);
  EXPECT_NEAR(SeparationDeg(with.rotation, AboutY(60.0)), 0.0, kSameRotationDeg);
}

/**
 * A rotation that arrives longer than unit does not thereby weigh more.
 *
 * `IsUsableRotation` admits any finite norm above 1e-12, so an input is not unit merely because it
 * passed the gate — and an unnormalised quaternion contributes its **squared** norm to the
 * outer-product sum, which is a second weight nobody asked for. A tripled quaternion would count
 * nine times.
 *
 * Reachable rather than theoretical: a relative rotation recovered from a fitted 3x3 matrix is unit
 * only to the precision of the fit, and the caller that will pass these is holding exactly that.
 * Tripled here rather than nudged, so the assertion is about the mechanism and not about a
 * tolerance.
 */
TEST(AverageQuaternions, ALongerThanUnitRotationDoesNotCountForMore) {
  const Quat ten = AboutY(10.0);
  const std::vector<Quat> pair{Quat{3 * ten.w, 3 * ten.x, 3 * ten.y, 3 * ten.z}, AboutY(50.0)};
  ASSERT_TRUE(IsUsableRotation(pair[0])) << "the tripled input does not reach the average at all";

  const QuaternionAverage average = AverageQuaternions(pair, {});
  ASSERT_TRUE(average.valid);
  EXPECT_NEAR(SeparationDeg(average.rotation, AboutY(30.0)), 0.0, kSameRotationDeg);

  // The number this would be instead, so the assertion above is pinned against the specific failure
  // rather than against "not 30": squared, the triple weighs nine.
  EXPECT_NEAR(ExpectedAngleDeg({10.0, 50.0}, {9.0, 1.0}), 13.765698, 1e-6);
}

/**
 * Scaling every weight by one factor leaves the answer alone.
 *
 * The objective is a quadratic form in the weights, so a common factor scales the matrix and not its
 * principal eigenvector. Worth asserting because it is what lets a caller pass raw inlier counts —
 * evidence, not a distribution — rather than having to normalise first, and because any threshold
 * written against an absolute weight would break it.
 */
TEST(AverageQuaternions, TheAnswerDependsOnTheRatioOfWeightsAndNotTheirScale) {
  const std::vector<Quat> pair{AboutY(10.0), AboutY(50.0)};
  const QuaternionAverage small = AverageQuaternions(pair, std::vector<double>{3.0, 1.0});
  const QuaternionAverage large = AverageQuaternions(pair, std::vector<double>{3.0e6, 1.0e6});
  ASSERT_TRUE(small.valid && large.valid);
  EXPECT_NEAR(SeparationDeg(small.rotation, large.rotation), 0.0, kSameRotationDeg);
}

/**
 * An empty weight span means equal weights, and says so by agreeing with explicit ones.
 *
 * The spelling that passes no weights is the one `rotation_scoring` uses, so the two have to be one
 * function rather than two paths that happen to agree today.
 */
TEST(AverageQuaternions, NoWeightsMeansEqualWeights) {
  const std::vector<Quat> three{AboutY(10.0), AboutY(50.0), AboutY(-20.0)};
  const QuaternionAverage implicitly = AverageQuaternions(three, {});
  const QuaternionAverage explicitly = AverageQuaternions(three, std::vector<double>{2, 2, 2});
  ASSERT_TRUE(implicitly.valid && explicitly.valid);
  EXPECT_NEAR(SeparationDeg(implicitly.rotation, explicitly.rotation), 0.0, kSameRotationDeg);

  // Against the closed form rather than only against each other, so two identical wrong answers
  // cannot satisfy this.
  EXPECT_NEAR(SeparationDeg(implicitly.rotation, AboutY(ExpectedAngleDeg({10, 50, -20}, {1, 1, 1}))),
              0.0, kSameRotationDeg);
}

/**
 * "Only the ratios matter" holds at every weight scale the gate admits, not just at ordinary ones.
 *
 * The header promises that weights are evidence rather than a distribution, so raw inlier counts may
 * be passed unnormalised. `TheAnswerDependsOnTheRatioOfWeightsAndNotTheirScale` checks that over a
 * factor of a million, which is the range a caller plausibly uses and is nowhere near where it broke.
 *
 * **It broke above a weight *total* of `sqrt(DBL_MAX)`**, and silently. The eigensolver's
 * off-diagonal test is relative to the squared trace, the trace is the weight total, and squaring a
 * total past 1.34e154 gives an infinity — so `offDiagonal <= 1e-30 * inf` is true on the first pass,
 * Jacobi breaks before rotating anything, and what comes back is whichever coordinate axis the scan
 * reached first, with `valid` and `isUnique` both true. Underflow does the same at the bottom, where
 * the squared entries round to zero. Reproduced here: at `6.71e153` each the
 * answer collapses onto the first input, 30 degrees from where it belongs.
 *
 * The fix is to divide by the largest weight before accumulating, so the entries are bounded by one
 * whatever scale the caller works in — which is what makes the promise true rather than true over a
 * range nobody wrote down.
 */
TEST(AverageQuaternions, TheRatioStillDecidesAtWeightsNearTheEdgeOfTheRange) {
  const std::vector<Quat> pair{AboutY(0.0), AboutY(60.0)};

  // Spot values rather than a sweep, each one chosen for where it sits: ordinary, the largest total
  // the old code survived, just past it, far past it, and the two underflow cases.
  // `DBL_MAX` is the entry that makes this test about the *divisor*. The rest stop at 1e300, whose
  // total over two inputs is 2e300 — nowhere near overflowing — so every one of them passes just as
  // happily if the implementation divides by the weight **total** instead of the largest. That
  // variant is the one the fix's own comment rules out, and at 1e308 it answers 30 degrees from
  // where it belongs with `valid` true. The top of the admissible range is where the two diverge,
  // and `AverageQuaternions` admits any finite non-negative weight.
  for (const double weight : {1.0, 1e100, 6.7039039644650269e153, 6.71e153, 1e200, 1e300,
                              std::numeric_limits<double>::max(),
                              1e-100, 4.76837e-162, 1e-170, 1e-300}) {
    const QuaternionAverage average = AverageQuaternions(pair, std::vector<double>{weight, weight});
    ASSERT_TRUE(average.valid) << "weight " << weight;
    EXPECT_NEAR(SeparationDeg(average.rotation, AboutY(30.0)), 0.0, kSameRotationDeg) << "weight " << weight;
  }

  // And an unequal pair at the top of the range still lands where the ratio says, so the fix is not
  // "clamp everything to equal weights".
  const QuaternionAverage lopsided =
      AverageQuaternions(pair, std::vector<double>{3e300, 1e300});
  ASSERT_TRUE(lopsided.valid);
  EXPECT_NEAR(SeparationDeg(lopsided.rotation, AboutY(ExpectedAngleDeg({0, 60}, {3, 1}))), 0.0,
              kSameRotationDeg);

  // **Everything above pins the scale and leaves the divisor's *rank* free**, which is the hole the
  // `DBL_MAX` entry was written to close and does not. Eleven of the twelve cases pass two equal
  // weights, where every candidate divisor — largest, total, smallest, first — is the same number;
  // the twelfth holds the ratio at three, where they differ by a factor of at most four and nothing
  // overflows either way. So dividing by the **smallest** non-zero weight, or by the **first**,
  // survives this test and the whole suite.
  //
  // A ratio the width of the range is what separates them, because the divisor decides which end of
  // the pair lands at one. Measured on `{1.0, 1e308}`: dividing by the largest gives entries of
  // `{1e-308, 1}` and the right answer; dividing by the smallest gives `{1, 1e308}`, whose squared
  // trace is an infinity, and the same silent collapse onto the first input — `valid` and `isUnique`
  // both true, sixty degrees from where it belongs — that the fix above removed. The mirrored pair
  // catches a "first non-zero" divisor at the other end.
  //
  // Not an exotic input: `AverageRotations(edges, anchors, 1e-6)` is in the shipped tests, and an
  // anchor weight against raw inlier counts is a ratio of millions before anyone tries.
  for (const std::vector<double>& ratio :
       {std::vector<double>{1.0, 1e308}, std::vector<double>{1e-300, 1.0}}) {
    const QuaternionAverage dominated = AverageQuaternions(pair, ratio);
    ASSERT_TRUE(dominated.valid) << ratio[0] << " " << ratio[1];
    EXPECT_NEAR(SeparationDeg(dominated.rotation, AboutY(60.0)), 0.0, kSameRotationDeg)
        << "the heavier of " << ratio[0] << " and " << ratio[1] << " did not decide the answer";
  }
}

/**
 * Two rotations exactly a half turn apart leave the maximiser a continuum, and the caller is told.
 *
 * `rotation_scoring.h` names this as the reachable degenerate case. What comes back is still *a*
 * maximiser — the objective genuinely has no single best answer here — so this is a flag beside a
 * usable value rather than a `valid` of false.
 */
TEST(AverageQuaternions, AHalfTurnApartIsReportedAsNotUnique) {
  const std::vector<Quat> opposed{AboutY(0.0), AboutY(180.0)};
  const QuaternionAverage average = AverageQuaternions(opposed, {});
  ASSERT_TRUE(average.valid);
  EXPECT_FALSE(average.isUnique);
}

/**
 * An ordinary spread is unique, so the flag above reports the degeneracy rather than the input
 * count.
 */
TEST(AverageQuaternions, AnOrdinarySpreadIsUnique) {
  const std::vector<Quat> three{AboutY(0.0), AboutY(20.0), AboutY(-15.0)};
  const QuaternionAverage average = AverageQuaternions(three, {});
  ASSERT_TRUE(average.valid);
  EXPECT_TRUE(average.isUnique);
}

/**
 * A near-tie is still a tie broken, which is what pins the threshold rather than its direction.
 *
 * The two cases above sit at the extremes — an exact tie and a spread whose eigenvalues differ by
 * most of their size — and between them they leave `kEigenvalueGap` free anywhere from zero to about
 * a half: moved to 0.5 and to 0.0, the whole suite stays green. That is a constant
 * carrying a measured claim with nothing measuring it.
 *
 * A tenth of a degree short of a half turn is what closes the gap. About one axis the two
 * eigenvalues are `1 ± cos(sep/2) sin(sep/2)`, so at 179.9 degrees the relative separation is
 * **1.744e-3** — six orders above the threshold and two below the 0.5 that used to pass, so the
 * constant is now pinned from both sides. It is the right answer as well as a convenient one: the
 * maximiser really is a single rotation here, and the comment on `kEigenvalueGap` claims only that
 * exact ties are caught.
 */
TEST(AverageQuaternions, ATenthOfADegreeShortOfAHalfTurnIsStillUnique) {
  const std::vector<Quat> nearly{AboutY(0.0), AboutY(179.9)};
  const QuaternionAverage average = AverageQuaternions(nearly, {});
  ASSERT_TRUE(average.valid);
  EXPECT_TRUE(average.isUnique)
      << "a relative eigenvalue gap of 1.7e-3 is being read as a tie";

  // The exact half turn beside it, so the pair says the threshold discriminates rather than that it
  // answers one way.
  const std::vector<Quat> exactly{AboutY(0.0), AboutY(180.0)};
  const QuaternionAverage tied = AverageQuaternions(exactly, {});
  ASSERT_TRUE(tied.valid);
  EXPECT_FALSE(tied.isUnique);

  // **And a tie that is a few ulps wide rather than bit-exact**, which is the case that makes this a
  // tolerance instead of an equality test. An exact half turn gives eigenvalues that are equal to
  // the bit, so `(top - second) > 0.0 * top` is false and a threshold of **zero** passes both cases
  // above. The solver does not reach this shape by being handed a literal 180: it reaches it through
  // accumulated Jacobi arithmetic, where the two eigenvalues land a few ulps apart and an equality
  // test calls the continuum unique.
  //
  // Measured at 179.999999999999 degrees: not unique at the committed 1e-12, unique at 0.0. So the
  // constant is bracketed from below as well as above, and an earlier answer of mine on this — that
  // being free down to zero was consistent with the comment's claim about exact ties — was wrong for
  // exactly this reason.
  const std::vector<Quat> almostTied{AboutY(0.0), AboutY(179.999999999999)};
  const QuaternionAverage barely = AverageQuaternions(almostTied, {});
  ASSERT_TRUE(barely.valid);
  EXPECT_FALSE(barely.isUnique)
      << "a tie a few ulps wide is being read as a single maximiser";

  // **The two arms above still leave eleven orders of magnitude free**, which is the correction this
  // paragraph exists for. Bracketed is not the same as pinned: the near-tie sets the floor at 2e-14
  // and the 179.9 arm sets the ceiling at its own 1.7e-3, so every value between — 1e-9, 1e-6, 1e-4,
  // 1e-3 — passes the whole suite. At 1e-4 the constant is a general conditioning test, which is
  // precisely what its declaration says it is not.
  //
  // This arm is the ceiling brought down to meet the floor. Relative gap is `sin(180 - sep)`, so a
  // ten-millionth of a degree short of a half turn gives **1.745e-9** — measured, not derived, and
  // the derivation is included only because it says which direction to move the literal. That leaves
  // the constant free over [2e-14, 1.7e-9] rather than [2e-14, 1.7e-3]: still a range, and now one
  // that contains no value at which the threshold would be doing a different job.
  const std::vector<Quat> hairsbreadth{AboutY(0.0), AboutY(179.9999999)};
  const QuaternionAverage sharp = AverageQuaternions(hairsbreadth, {});
  ASSERT_TRUE(sharp.valid);
  EXPECT_TRUE(sharp.isUnique) << "a relative eigenvalue gap of 1.7e-9 is being read as a tie";
}

// ----------------------------------------------------------------- refusals
//
// Each way of having no answer is asserted on its own rather than in a loop over "bad inputs": a
// loop proves that something refused and not that the right thing did. The identity is what a
// silent failure here would return, and the identity is a rotation a caller cannot tell from a
// measurement.

TEST(AverageQuaternions, NoRotationsIsARefusal) {
  const std::vector<Quat> none;
  EXPECT_FALSE(AverageQuaternions(none, {}).valid);
}

TEST(AverageQuaternions, AWeightSpanOfTheWrongLengthIsARefusal) {
  const std::vector<Quat> pair{AboutY(10.0), AboutY(50.0)};
  EXPECT_FALSE(AverageQuaternions(pair, std::vector<double>{1.0}).valid);
  EXPECT_FALSE(AverageQuaternions(pair, std::vector<double>{1.0, 1.0, 1.0}).valid);
}

TEST(AverageQuaternions, AQuaternionThatIsNotARotationIsARefusal) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<Quat> zeroed{AboutY(10.0), Quat{0, 0, 0, 0}};
  const std::vector<Quat> notANumber{AboutY(10.0), Quat{nan, 0, 0, 0}};
  EXPECT_FALSE(AverageQuaternions(zeroed, {}).valid);
  EXPECT_FALSE(AverageQuaternions(notANumber, {}).valid);
}

TEST(AverageQuaternions, AWeightThatIsNotAMeasurementIsARefusal) {
  const std::vector<Quat> pair{AboutY(10.0), AboutY(50.0)};
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();
  // Three against minus one, not one against minus one, and the difference is the whole test. The
  // pair that sums to zero is refused by the all-weights-zero gate instead, so with the negative
  // check deleted it still comes back false and this case passes without ever reaching the line it
  // is named for. Caught by sabotage: removing `w < 0.0` failed nothing until the total went
  // positive.
  EXPECT_FALSE(AverageQuaternions(pair, std::vector<double>{3.0, -1.0}).valid)
      << "a negative weight is not less evidence, it is evidence for the opposite";
  EXPECT_FALSE(AverageQuaternions(pair, std::vector<double>{1.0, nan}).valid);
  EXPECT_FALSE(AverageQuaternions(pair, std::vector<double>{1.0, infinity}).valid);
}

/**
 * Weights that are all zero are a refusal, and not the average of nothing dressed as a rotation.
 *
 * Reachable: a caller weighting by inlier count, over a set of pairs that were every one refused,
 * passes exactly this. The outer-product sum is the zero matrix, whose principal eigenvector is
 * whatever the eigensolver's scan order reaches first — so without this gate the answer is an
 * arbitrary axis with `valid` true.
 */
TEST(AverageQuaternions, EveryWeightZeroIsARefusal) {
  const std::vector<Quat> pair{AboutY(10.0), AboutY(50.0)};
  EXPECT_FALSE(AverageQuaternions(pair, std::vector<double>{0.0, 0.0}).valid);
}

}  // namespace
}  // namespace sphanorama
