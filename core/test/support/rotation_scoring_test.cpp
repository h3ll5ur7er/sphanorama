#include "support/rotation_scoring.h"

#include <cmath>
#include <algorithm>
#include <limits>
#include <random>
#include <vector>

#include <gtest/gtest.h>

#include "utilities/quaternion.h"

namespace sphanorama::test {
namespace {

constexpr double kDegPerRad = 57.295779513082320876798154814105;

Quat Yaw(double deg) { return FromAxisAngle(Vec3{0, 1, 0}, deg / kDegPerRad); }
Quat Pitch(double deg) { return FromAxisAngle(Vec3{1, 0, 0}, deg / kDegPerRad); }
Quat Roll(double deg) { return FromAxisAngle(Vec3{0, 0, 1}, deg / kDegPerRad); }

// A ring of frames, the shape a coverage plan actually produces.
std::vector<Quat> ARing(int count) {
  std::vector<Quat> ring;
  for (int i = 0; i < count; ++i) {
    ring.push_back(Multiply(Yaw(360.0 * i / count), Pitch(i % 3 == 0 ? 12.0 : -7.0)));
  }
  return ring;
}

// The gauge acts on the left, because Quat is device -> world and the freedom is in the world
// frame. Turning the whole reconstruction is left-multiplying every frame by one rotation.
std::vector<Quat> TurnedBy(const Quat& gauge, const std::vector<Quat>& frames) {
  std::vector<Quat> turned;
  for (const Quat& q : frames) turned.push_back(Multiply(gauge, q));
  return turned;
}

double DegBetween(const Quat& a, const Quat& b) { return AngleBetween(a, b) * kDegPerRad; }

TEST(RotationScoring, TruthScoredAgainstItselfIsZeroEverywhere) {
  const std::vector<Quat> truth = ARing(8);

  const RotationScore score = ScoreRotations(truth, truth);

  ASSERT_TRUE(score.valid);
  ASSERT_EQ(score.perFrameDeg.size(), truth.size());
  for (double deg : score.perFrameDeg) EXPECT_NEAR(deg, 0.0, 1e-9);
  EXPECT_NEAR(score.medianDeg, 0.0, 1e-9);
  EXPECT_NEAR(score.maxDeg, 0.0, 1e-9);
}

// The reason this file exists. Every frame is wrong by the same 30 degrees and the reconstruction
// is perfect: those are the same statement, and a scorer that cannot say so is measuring the
// coordinate system rather than the registration.
TEST(RotationScoring, AWholeReconstructionTurnedByOneRotationScoresZero) {
  const std::vector<Quat> truth = ARing(8);
  const Quat gauge = Multiply(Yaw(30.0), Roll(12.0));
  const std::vector<Quat> estimated = TurnedBy(Conjugate(gauge), truth);

  const RotationScore score = ScoreRotations(estimated, truth);

  ASSERT_TRUE(score.valid);
  for (double deg : score.perFrameDeg) EXPECT_NEAR(deg, 0.0, 1e-9);
  EXPECT_NEAR(DegBetween(score.alignment, gauge), 0.0, 1e-9);
}

// The previous test proves nothing unless the un-quotiented comparison would have failed it. This
// is that check, written out rather than assumed: without removing the gauge, every frame reads as
// wrong by the gauge's own angle.
TEST(RotationScoring, WithoutTheGaugeThatSameReconstructionWouldReadAsWrongOnEveryFrame) {
  const std::vector<Quat> truth = ARing(8);
  const Quat gauge = Yaw(30.0);
  const std::vector<Quat> estimated = TurnedBy(Conjugate(gauge), truth);

  for (size_t i = 0; i < truth.size(); ++i) {
    EXPECT_NEAR(DegBetween(estimated[i], truth[i]), 30.0, 1e-9);
  }
}

// With one pair there is always a gauge that lands the estimate exactly on the truth, so nothing
// is left to measure. A dataset needs frames to be accurate *relative to*.
TEST(RotationScoring, ASingleFrameAlwaysScoresZeroBecauseTheGaugeAbsorbsAllOfIt) {
  const std::vector<Quat> truth{Yaw(15.0)};
  const std::vector<Quat> estimated{Multiply(Pitch(80.0), Yaw(15.0))};

  const RotationScore score = ScoreRotations(estimated, truth);

  ASSERT_TRUE(score.valid);
  ASSERT_EQ(score.perFrameDeg.size(), 1u);
  EXPECT_NEAR(score.perFrameDeg[0], 0.0, 1e-9);
}

// Two frames pin down exactly one thing: how they sit relative to each other. The gauge takes the
// common half, so a relative error of theta is reported as theta/2 on each — which is the honest
// answer, because neither frame is more wrong than the other.
TEST(RotationScoring, TwoFramesSplitTheirRelativeErrorBetweenThem) {
  const std::vector<Quat> truth{Quat{}, Yaw(40.0)};
  const std::vector<Quat> estimated{Quat{}, Yaw(50.0)};

  const RotationScore score = ScoreRotations(estimated, truth);

  ASSERT_TRUE(score.valid);
  EXPECT_NEAR(score.perFrameDeg[0], 5.0, 1e-6);
  EXPECT_NEAR(score.perFrameDeg[1], 5.0, 1e-6);
  EXPECT_NEAR(score.medianDeg, 5.0, 1e-6);
}

TEST(RotationScoring, OneFrameOffByItselfShowsInTheMaxAndLeavesTheMedianAlone) {
  const std::vector<Quat> truth = ARing(9);
  std::vector<Quat> estimated = truth;
  estimated[4] = Multiply(Pitch(20.0), estimated[4]);

  const RotationScore score = ScoreRotations(estimated, truth);

  ASSERT_TRUE(score.valid);
  EXPECT_LT(score.medianDeg, 3.0);           // the outlier cannot move the middle value
  EXPECT_GT(score.maxDeg, 15.0);             // and it is still visible in the worst case
  EXPECT_GT(score.maxDeg, score.medianDeg);
}

// q and -q are the same rotation. A sensor stream that flips sign mid-capture is a real thing, and
// it must not read as a 180-degree jump on a frame that never moved.
//
// Exactly half the frames are flipped, and that number is the point rather than an arbitrary
// choice. Sign invariance here comes from averaging residuals as outer products; the obvious
// alternative — summing the residual quaternions directly — survives any other split, because four
// residuals pointing one way outvote two pointing the other and the normalised sum still lands on
// the right rotation. At a three-three split the naive sum cancels to zero exactly, and only then
// does the difference between the two implementations become visible. A gauge is applied for the
// same reason: without one every residual is the identity, and a zero sum falls back to the
// identity, which is the right answer by accident.
TEST(RotationScoring, NegatingHalfTheQuaternionsChangesNothing) {
  const std::vector<Quat> truth = ARing(6);
  const Quat gauge = Multiply(Yaw(60.0), Pitch(20.0));
  std::vector<Quat> estimated = TurnedBy(Conjugate(gauge), truth);
  const auto negate = [](const Quat& q) { return Quat{-q.w, -q.x, -q.y, -q.z}; };
  estimated[1] = negate(estimated[1]);
  estimated[3] = negate(estimated[3]);
  estimated[5] = negate(estimated[5]);

  const RotationScore score = ScoreRotations(estimated, truth);

  ASSERT_TRUE(score.valid);
  for (double deg : score.perFrameDeg) EXPECT_NEAR(deg, 0.0, 1e-9);
  EXPECT_NEAR(DegBetween(score.alignment, gauge), 0.0, 1e-9);

  // And in the truth set too, which is a different code path into the same residual.
  std::vector<Quat> negatedTruth = truth;
  negatedTruth[0] = negate(truth[0]);
  negatedTruth[2] = negate(truth[2]);
  negatedTruth[4] = negate(truth[4]);
  const RotationScore fromTruthSide = ScoreRotations(TurnedBy(Conjugate(gauge), truth), negatedTruth);
  ASSERT_TRUE(fromTruthSide.valid);
  for (double deg : fromTruthSide.perFrameDeg) EXPECT_NEAR(deg, 0.0, 1e-9);
}

// The alignment claims to be the best one. This checks it by trying to beat it: nudge it in six
// directions and the objective it maximises has to get worse in all of them.
//
// The gauge here is large on purpose, and that is the whole difference between this test and a
// vacuous one. The first version applied no gauge, so the answer sat 0.29 degrees from the identity
// — inside the resolution of the 0.5-degree probe below, which meant a nudge in the *right*
// direction still landed further away once its perpendicular component was counted. Hardcoding the
// alignment to the identity passed it. With the estimate turned by 55 degrees, an implementation
// that does not actually search fails on the first assertion.
TEST(RotationScoring, NoNearbyRotationAlignsBetterThanTheOneChosen) {
  const std::vector<Quat> truth = ARing(7);
  const Quat gauge = Multiply(Yaw(55.0), Roll(-24.0));
  std::vector<Quat> estimated;
  for (size_t i = 0; i < truth.size(); ++i) {
    // Errors large enough that the alignment is a genuine compromise rather than exact.
    const double wobble = (i % 2 == 0) ? 4.0 : -6.0;
    estimated.push_back(Multiply(FromAxisAngle(Vec3{0.3, 0.8, -0.5}, wobble / kDegPerRad),
                                 Multiply(Conjugate(gauge), truth[i])));
  }

  const GaugeAlignment best = BestGaugeAlignment(estimated, truth);
  ASSERT_TRUE(best.valid);

  // The objective: sum of squared quaternion dot products between the aligned estimate and truth.
  const auto objective = [&](const Quat& g) {
    double total = 0;
    for (size_t i = 0; i < truth.size(); ++i) {
      const Quat aligned = Normalize(Multiply(g, estimated[i]));
      const Quat t = Normalize(truth[i]);
      const double dot = aligned.w * t.w + aligned.x * t.x + aligned.y * t.y + aligned.z * t.z;
      total += dot * dot;
    }
    return total;
  };

  const double atBest = objective(best.rotation);
  const Vec3 axes[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (const Vec3& axis : axes) {
    for (double nudge : {0.5, -0.5}) {
      const Quat perturbed = Multiply(FromAxisAngle(axis, nudge / kDegPerRad), best.rotation);
      EXPECT_LT(objective(perturbed), atBest) << "a nudge beat the chosen alignment";
    }
  }
  // Local optimality is not optimality. A rotation far from the chosen one must not do better
  // either, and only a global check can say so.
  std::mt19937_64 rng(11071988);
  std::normal_distribution<double> gaussian(0.0, 1.0);
  for (int trial = 0; trial < 2000; ++trial) {
    const Quat far = Normalize(Quat{gaussian(rng), gaussian(rng), gaussian(rng), gaussian(rng)});
    EXPECT_LT(objective(far), atBest + 1e-9);
  }
}

// The regime that decided how this is computed. Residuals with no common direction leave the
// eigenvalue gap near zero, which is where power iteration — the obvious implementation — stops
// converging: on wholly unrelated estimates it exhausts a 200-iteration budget 45.6% of the time at
// 60 frames, while Jacobi finishes in five working sweeps at every size measured. That is not
// observable from outside, so what this pins is the consequence: the alignment is still the best one
// even here. (An earlier version of this comment cited half-garbage input instead, which was a rare
// tail stated as the norm — see ADR 0049.)
TEST(RotationScoring, TheAlignmentIsStillTheBestOneWhenHalfTheEstimatesAreWorthless) {
  std::mt19937_64 rng(20260908);
  std::normal_distribution<double> gaussian(0.0, 1.0);
  const auto randomRotation = [&] {
    return Normalize(Quat{gaussian(rng), gaussian(rng), gaussian(rng), gaussian(rng)});
  };

  const std::vector<Quat> truth = ARing(12);
  const Quat gauge = Multiply(Yaw(63.0), Pitch(21.0));
  std::vector<Quat> estimated;
  for (size_t i = 0; i < truth.size(); ++i) {
    estimated.push_back(i % 2 == 0 ? Multiply(Conjugate(gauge), truth[i]) : randomRotation());
  }

  const GaugeAlignment best = BestGaugeAlignment(estimated, truth);
  ASSERT_TRUE(best.valid);

  const auto objective = [&](const Quat& g) {
    double total = 0;
    for (size_t i = 0; i < truth.size(); ++i) {
      const Quat aligned = Normalize(Multiply(g, estimated[i]));
      const Quat t = Normalize(truth[i]);
      const double dot = aligned.w * t.w + aligned.x * t.x + aligned.y * t.y + aligned.z * t.z;
      total += dot * dot;
    }
    return total;
  };

  const double atBest = objective(best.rotation);
  const Vec3 axes[3]{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
  for (const Vec3& axis : axes) {
    for (double nudge : {0.25, -0.25, 3.0, -3.0}) {
      const Quat perturbed = Multiply(FromAxisAngle(axis, nudge / kDegPerRad), best.rotation);
      EXPECT_LT(objective(perturbed), atBest) << "a nudge beat the chosen alignment";
    }
  }
  // And nothing far away does better either, which a local check alone cannot say.
  for (int trial = 0; trial < 2000; ++trial) {
    EXPECT_LT(objective(randomRotation()), atBest + 1e-9);
  }
}

// The alignment has to be found rather than assumed, so a reconstruction whose frames are all
// wrong by a large common rotation still scores its residual correctly.
TEST(RotationScoring, ALargeGaugeIsRemovedJustAsCompletelyAsASmallOne) {
  const std::vector<Quat> truth = ARing(8);
  const Quat gauge = Multiply(Yaw(170.0), Pitch(85.0));
  std::vector<Quat> estimated = TurnedBy(Conjugate(gauge), truth);
  estimated[3] = Multiply(Roll(2.0), estimated[3]);

  const RotationScore score = ScoreRotations(estimated, truth);

  ASSERT_TRUE(score.valid);
  EXPECT_NEAR(score.medianDeg, 0.0, 0.5);
  EXPECT_LT(score.maxDeg, 2.5);
}

// The per-frame errors come back against the frames that produced them. Nothing pinned this: a
// reviewer appended `std::reverse` to the vector and all fifteen tests stayed green, because the
// only test that breaks a specific frame never asserts *which* entry went bad.
TEST(RotationScoring, ThePerFrameErrorsComeBackInTheOrderTheFramesWereGiven) {
  const std::vector<Quat> truth = ARing(6);
  std::vector<Quat> estimated = truth;
  estimated[1] = Multiply(Roll(4.0), estimated[1]);
  estimated[4] = Multiply(Pitch(16.0), estimated[4]);

  const RotationScore score = ScoreRotations(estimated, truth);

  ASSERT_TRUE(score.valid);
  ASSERT_EQ(score.perFrameDeg.size(), 6u);
  // Index 4 was broken hardest, index 1 next, and the four untouched frames sit below both.
  EXPECT_GT(score.perFrameDeg[4], score.perFrameDeg[1]);
  for (size_t i : {0u, 2u, 3u, 5u}) {
    EXPECT_LT(score.perFrameDeg[i], score.perFrameDeg[1]) << "untouched frame " << i;
    EXPECT_LT(score.perFrameDeg[i], score.perFrameDeg[4]) << "untouched frame " << i;
  }
}

// A registration error is in the camera's own frame, not the world's, and that distinction is what
// makes the frames themselves matter.
//
// It was found the hard way. Every other test here perturbs the estimate on the *left*, and a
// left-multiplied error cancels the frame out entirely: `Residual(g (x) t, t)` is
// `t (x) conj(t) (x) conj(g)`, which is `conj(g)` whatever `t` was. So M came out rank one, the
// eigensolver saw the same matrix regardless of the ring, and a reviewer proved the point by
// replacing every frame in `ARing` with the identity — all fifteen tests still passed. The fixture
// was decorative.
//
// Right-multiplying puts the frame back in: `Residual(t (x) e, t)` is `t (x) conj(e) (x) conj(t)`,
// which is the error seen from that frame's orientation and differs for every frame. This is the
// only test here whose arrangement the eigensolver cannot reduce to a single residual.
TEST(RotationScoring, AnErrorInEachCamerasOwnFrameMakesTheFramesThemselvesMatter) {
  const std::vector<Quat> truth = ARing(8);
  // Large enough that the signal is unmistakable. Conjugation preserves a rotation's *angle*, so
  // every residual here is the same size and differs only in axis; the spread that reaches the
  // score therefore grows with the error rather than tracking it. Measured against this ring:
  // 0.03 degrees of spread at a 5-degree error, 0.94 at 30, 4.18 at 60.
  const Quat localError = Multiply(Roll(60.0), Pitch(22.0));
  std::vector<Quat> estimated;
  for (const Quat& q : truth) estimated.push_back(Multiply(q, localError));

  const RotationScore score = ScoreRotations(estimated, truth);
  ASSERT_TRUE(score.valid);

  double smallest = score.perFrameDeg[0];
  double largest = score.perFrameDeg[0];
  for (double deg : score.perFrameDeg) {
    smallest = std::min(smallest, deg);
    largest = std::max(largest, deg);
  }
  // Identical frames give a spread of exactly zero here — measured, not approximately zero — so
  // this is the assertion that fails the moment `ARing` stops producing distinct orientations.
  EXPECT_GT(largest - smallest, 1.0) << "the per-frame errors do not depend on the frames";

  // And the control: the same error applied on the left collapses to one residual and every frame
  // reports the identical number.
  std::vector<Quat> leftInstead;
  for (const Quat& q : truth) leftInstead.push_back(Multiply(localError, q));
  const RotationScore collapsed = ScoreRotations(leftInstead, truth);
  ASSERT_TRUE(collapsed.valid);
  for (double deg : collapsed.perFrameDeg) EXPECT_NEAR(deg, collapsed.perFrameDeg[0], 1e-9);
}

// Two frames exactly 180 degrees apart leave the top two eigenvalues equal, so every rotation in a
// whole plane maximises the objective and the one returned depends on input order. The score is
// still *a* maximiser; what it is not is a unique answer, and the caller is told.
TEST(RotationScoring, AnAlignmentThatCouldHaveBeenAnythingSaysSo) {
  const std::vector<Quat> truth{Quat{}, Yaw(40.0)};
  const std::vector<Quat> estimated{Quat{}, Multiply(Yaw(180.0), Yaw(40.0))};

  const RotationScore score = ScoreRotations(estimated, truth);

  ASSERT_TRUE(score.valid);
  EXPECT_FALSE(score.alignmentIsUnique);

  // The reason it matters: the reported distribution is order-dependent. Both readings below are
  // legitimate maximisers, and neither is the even 90/90 split a reader would assume.
  const std::vector<Quat> swappedTruth{truth[1], truth[0]};
  const std::vector<Quat> swappedEstimate{estimated[1], estimated[0]};
  const RotationScore swapped = ScoreRotations(swappedEstimate, swappedTruth);
  ASSERT_TRUE(swapped.valid);
  EXPECT_FALSE(swapped.alignmentIsUnique);
  EXPECT_NEAR(score.perFrameDeg[0] + score.perFrameDeg[1], 180.0, 1e-6);
  EXPECT_NEAR(swapped.perFrameDeg[0] + swapped.perFrameDeg[1], 180.0, 1e-6);
  EXPECT_NEAR(score.perFrameDeg[0], swapped.perFrameDeg[1], 1e-6);
}

TEST(RotationScoring, AnOrdinaryReconstructionsAlignmentIsUnique) {
  const std::vector<Quat> truth = ARing(8);
  std::vector<Quat> estimated = TurnedBy(Conjugate(Yaw(25.0)), truth);
  estimated[3] = Multiply(Roll(2.0), estimated[3]);

  const RotationScore score = ScoreRotations(estimated, truth);

  ASSERT_TRUE(score.valid);
  EXPECT_TRUE(score.alignmentIsUnique);
}

// A quaternion that is a rotation but not a unit one scores identically.
//
// **This test does not distinguish the two implementations of that**, and saying so is the point.
// A reviewer read the per-frame loop as an overflow: `IsUsableRotation` admits norms up to
// sqrt(DBL_MAX), so a product whose squares overflow would have `Normalize` substitute the identity
// and measure against that. Not reachable — `Multiply` is exactly norm-multiplicative, so the gate
// and the product overflow at the same threshold; 133,266 quaternions straddling it and 400,000 in
// the small-norm band produced zero differences. Removing the explicit `Normalize` this test was
// written for breaks nothing, because `AngleBetween` normalises internally anyway.
//
// So what is pinned here is the promise, not the mechanism: scale invariance is a property of the
// score, and this fails the day `AngleBetween` stops providing it. The explicit `Normalize` in the
// implementation is not a guard against a reachable state — it is there so the line is correct
// without reading another file, which is a different thing and worth keeping for a different
// reason.
TEST(RotationScoring, AScaledQuaternionIsTheSameRotationAndScoresTheSame) {
  const std::vector<Quat> truth = ARing(5);
  std::vector<Quat> unit = TurnedBy(Conjugate(Yaw(18.0)), truth);
  unit[2] = Multiply(Pitch(11.0), unit[2]);

  const RotationScore reference = ScoreRotations(unit, truth);
  ASSERT_TRUE(reference.valid);

  const auto scale = [](const Quat& q, double k) { return Quat{q.w * k, q.x * k, q.y * k, q.z * k}; };
  for (double k : {1e-11, 1e-3, 7.0, 1e6, 1e120}) {
    std::vector<Quat> scaled = unit;
    for (size_t i = 0; i < scaled.size(); ++i) scaled[i] = scale(scaled[i], k);
    const RotationScore score = ScoreRotations(scaled, truth);
    ASSERT_TRUE(score.valid) << "scale " << k;
    for (size_t i = 0; i < score.perFrameDeg.size(); ++i) {
      EXPECT_NEAR(score.perFrameDeg[i], reference.perFrameDeg[i], 1e-9) << "scale " << k;
    }
  }
}

// The median is steadier than the mean and is not immune, and the header now says so. This pins the
// behaviour rather than the wish: one frame turned right around still moves the median, by an
// amount that falls roughly as 1/N and is around a degree at the sizes a real sphere plans.
TEST(RotationScoring, AnOutlierMovesTheMedianToo) {
  struct Case { int frames; double lower; double upper; };
  // Measured, one frame turned by 120 degrees and the rest exact.
  const Case cases[]{{9, 6.0, 7.2}, {31, 1.4, 2.0}, {61, 0.7, 1.0}};

  for (const Case& c : cases) {
    const std::vector<Quat> truth = ARing(c.frames);
    std::vector<Quat> estimated = truth;
    estimated[c.frames / 2] = Multiply(Pitch(120.0), estimated[c.frames / 2]);

    const RotationScore score = ScoreRotations(estimated, truth);

    ASSERT_TRUE(score.valid);
    EXPECT_GT(score.medianDeg, c.lower) << c.frames << " frames";
    EXPECT_LT(score.medianDeg, c.upper) << c.frames << " frames";
    // Still far steadier than the mean, which is the reason it is the headline number.
    EXPECT_LT(score.medianDeg, score.meanDeg) << c.frames << " frames";
  }
}

TEST(RotationScoring, MismatchedLengthsAreRefused) {
  const RotationScore score = ScoreRotations(ARing(4), ARing(5));

  EXPECT_FALSE(score.valid);
  EXPECT_TRUE(score.perFrameDeg.empty());
}

TEST(RotationScoring, AnEmptyReconstructionIsRefused) {
  const RotationScore score = ScoreRotations({}, {});

  EXPECT_FALSE(score.valid);
}

TEST(RotationScoring, AQuaternionThatIsNotARotationIsRefused) {
  const std::vector<Quat> truth = ARing(3);

  std::vector<Quat> zeroed = truth;
  zeroed[1] = Quat{0, 0, 0, 0};
  EXPECT_FALSE(ScoreRotations(zeroed, truth).valid);

  std::vector<Quat> notANumber = truth;
  notANumber[2] = Quat{std::numeric_limits<double>::quiet_NaN(), 0, 0, 0};
  EXPECT_FALSE(ScoreRotations(notANumber, truth).valid);

  // And in the truth set too — a dataset with a broken ground truth is not a hard scoring problem,
  // it is a dataset nobody should score against.
  std::vector<Quat> brokenTruth = truth;
  brokenTruth[0] = Quat{0, 0, 0, 0};
  EXPECT_FALSE(ScoreRotations(truth, brokenTruth).valid);
}

TEST(RotationScoring, TheMedianOfAnEvenCountIsTheMeanOfTheTwoMiddleValues) {
  const std::vector<Quat> truth = ARing(4);
  std::vector<Quat> estimated = truth;
  estimated[1] = Multiply(Roll(2.0), estimated[1]);
  estimated[2] = Multiply(Pitch(6.0), estimated[2]);
  estimated[3] = Multiply(Yaw(12.0), estimated[3]);

  const RotationScore score = ScoreRotations(estimated, truth);

  ASSERT_TRUE(score.valid);
  std::vector<double> sorted = score.perFrameDeg;
  std::sort(sorted.begin(), sorted.end());

  // A gap big enough that averaging is doing visible work, asserted rather than assumed. The first
  // version of this test used `EXPECT_NE(sorted[1], sorted[2])` to make that point and the two
  // values turned out to differ by 1e-12 — so the assertion below held whether the median averaged
  // the middle pair or just took the upper one, and a sabotage that removed the averaging broke
  // nothing at all. EXPECT_NE on floating point is nearly always true and says nothing about
  // whether a difference is meaningful.
  ASSERT_GT(sorted[2] - sorted[1], 1.0);
  EXPECT_NEAR(score.medianDeg, (sorted[1] + sorted[2]) * 0.5, 1e-9);
}

TEST(RotationScoring, TheMeanAndMaxDescribeTheSamePerFrameNumbers) {
  const std::vector<Quat> truth = ARing(5);
  std::vector<Quat> estimated = truth;
  estimated[1] = Multiply(Pitch(3.0), estimated[1]);
  estimated[4] = Multiply(Yaw(9.0), estimated[4]);

  const RotationScore score = ScoreRotations(estimated, truth);

  ASSERT_TRUE(score.valid);
  double sum = 0;
  double worst = 0;
  for (double deg : score.perFrameDeg) {
    sum += deg;
    worst = std::max(worst, deg);
  }
  EXPECT_NEAR(score.meanDeg, sum / static_cast<double>(score.perFrameDeg.size()), 1e-12);
  EXPECT_NEAR(score.maxDeg, worst, 1e-12);
}

}  // namespace
}  // namespace sphanorama::test
