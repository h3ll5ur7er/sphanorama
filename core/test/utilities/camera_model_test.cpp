// The lens as maths. Nothing here is knowable by eye — a focal length that is out by a factor of
// two still renders a plausible-looking frame, and a flipped Y axis still stitches, upside down.
// So these are invariants rather than expected outputs (docs/00 §0.2): a direction projected and
// unprojected is the same direction, the edge of the field of view is the edge of the image, and
// every way of having no answer is a refusal rather than a pixel.
#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "utilities/camera_model.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegPerRad = 180.0 / kPi;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// A 66-degree lens over a 960x1280 portrait frame — the shape the app assumes when a browser will
// not say (see CaptureSessionManager), so the numbers in these tests are the ones it runs on.
Intrinsics Phone() { return LensFromFieldOfView(66.0, 50.0, 960, 1280); }

// A direction `degrees` off the forward axis, turned toward +X (right) when `axis` is 0 and
// toward +Y (up) when it is 1. Built from sin/cos directly rather than through a quaternion, so a
// bug in the rotation utilities cannot make this test agree with itself.
Vec3 OffAxis(double degrees, int axis) {
  const double t = degrees / kDegPerRad;
  return axis == 0 ? Vec3{std::sin(t), 0, -std::cos(t)} : Vec3{0, std::sin(t), -std::cos(t)};
}

// A lens with every distortion term non-zero, and strong enough that ignoring the coefficients
// moves a corner by tens of pixels rather than by rounding.
Intrinsics Distorted() {
  Intrinsics lens = Phone();
  lens.k1 = -0.28;
  lens.k2 = 0.12;
  lens.k3 = -0.02;
  lens.p1 = 0.001;
  lens.p2 = -0.0015;
  return lens;
}

// ------------------------------------------------------------------ what counts as a lens

TEST(IsUsableLens, TheIntrinsicsACaptureHoldsTodayAreNotALens) {
  // CaptureSessionManager fills in width and height and nothing else, because the focal length is
  // estimated by a build that has not happened. If this ever answers true, something has started
  // trusting a zero focal length as a measurement.
  Intrinsics held;
  held.width = 960;
  held.height = 1280;
  EXPECT_FALSE(IsUsableLens(held));
  EXPECT_FALSE(IsUsableLens(Intrinsics{}));
}

TEST(IsUsableLens, ALensNeedsAnImageWithArea) {
  Intrinsics lens = Phone();
  lens.width = 0;
  EXPECT_FALSE(IsUsableLens(lens));
  lens = Phone();
  lens.height = -1280;
  EXPECT_FALSE(IsUsableLens(lens));
}

TEST(IsUsableLens, ALensNeedsItsOpticalCentreInsideItsImage) {
  // Not fussiness about realism: without it the field of view collides with its own silence. Each
  // half-angle is measured from the optical centre to an edge, so a centre far outside the image
  // makes one of them negative and nearly cancels the other — at cx = 1e11 both atans round to the
  // same double just under pi/2 and `HorizontalFovDeg` returns exactly 0.0, which is the contract's
  // word for "will not say" (types.h), for a lens this function had accepted.
  Intrinsics lens = Phone();
  lens.cx = 1e11;
  EXPECT_FALSE(IsUsableLens(lens));
  EXPECT_DOUBLE_EQ(HorizontalFovDeg(lens), 0.0);

  for (double cx : {-1.0, 0.0, 960.0, 961.0}) {
    Intrinsics outside = Phone();
    outside.cx = cx;
    EXPECT_FALSE(IsUsableLens(outside)) << "cx=" << cx;
  }
  Intrinsics low = Phone();
  low.cy = -0.5;
  EXPECT_FALSE(IsUsableLens(low));

  // Off centre but inside is a perfectly ordinary calibration and stays a lens.
  Intrinsics decentred = Phone();
  decentred.cx = 300.0;
  EXPECT_TRUE(IsUsableLens(decentred));
}

TEST(IsUsableLens, ALensNeedsAPositiveFocalLengthOnBothAxes) {
  Intrinsics lens = Phone();
  lens.fx = 0;
  EXPECT_FALSE(IsUsableLens(lens));
  lens = Phone();
  lens.fy = -800;
  EXPECT_FALSE(IsUsableLens(lens));
}

TEST(IsUsableLens, AFieldThatIsNotANumberIsNotALens) {
  // Every one of these arrives from JavaScript as a double, where NaN and Infinity are ordinary
  // values, and each of them slips past a "> 0" test on its own.
  const double bad[] = {kNaN, kInf, -kInf};
  for (double value : bad) {
    Intrinsics lens = Phone();
    lens.fx = value;
    EXPECT_FALSE(IsUsableLens(lens)) << "fx";
    lens = Phone();
    lens.cx = value;
    EXPECT_FALSE(IsUsableLens(lens)) << "cx";
    lens = Phone();
    lens.k1 = value;
    EXPECT_FALSE(IsUsableLens(lens)) << "k1";
    lens = Phone();
    lens.p2 = value;
    EXPECT_FALSE(IsUsableLens(lens)) << "p2";
  }
}

// ---------------------------------------------------- building one from a field of view

TEST(LensFromFieldOfView, AFieldOfViewSurvivesTheTripThroughAFocalLength) {
  // The round trip is the whole test: a focal length is only ever right relative to the angle it
  // was meant to subtend, and neither number can be checked by looking at it.
  const Intrinsics lens = Phone();
  ASSERT_TRUE(IsUsableLens(lens));
  EXPECT_NEAR(HorizontalFovDeg(lens), 66.0, 1e-9);
  EXPECT_NEAR(VerticalFovDeg(lens), 50.0, 1e-9);
}

TEST(LensFromFieldOfView, TheOpticalCentreIsTheMiddleOfTheImage) {
  const Intrinsics lens = Phone();
  EXPECT_DOUBLE_EQ(lens.cx, 480.0);
  EXPECT_DOUBLE_EQ(lens.cy, 640.0);
}

TEST(LensFromFieldOfView, AnAngleThatIsNotALensYieldsNoLens) {
  const double bad[] = {0.0, -66.0, 180.0, 181.0, kNaN, kInf};
  for (double angle : bad) {
    EXPECT_FALSE(IsUsableLens(LensFromFieldOfView(angle, 50.0, 960, 1280))) << angle;
    EXPECT_FALSE(IsUsableLens(LensFromFieldOfView(66.0, angle, 960, 1280))) << angle;
  }
  EXPECT_FALSE(IsUsableLens(LensFromFieldOfView(66.0, 50.0, 0, 1280)));
  EXPECT_FALSE(IsUsableLens(LensFromFieldOfView(66.0, 50.0, 960, -1)));
}

TEST(LensFromFieldOfView, TheLensItInventsDoesNotClaimToHaveBeenMeasured) {
  EXPECT_FALSE(Phone().estimated);
}

TEST(HorizontalFovDeg, AnUnusableLensSaysNothingRatherThanZeroDegrees) {
  // 0 is the contract's silence, not a measurement of a pinhole. The planner already refuses it.
  EXPECT_DOUBLE_EQ(HorizontalFovDeg(Intrinsics{}), 0.0);
  EXPECT_DOUBLE_EQ(VerticalFovDeg(Intrinsics{}), 0.0);
}

TEST(HorizontalFovDeg, TheAngleIsMeasuredThroughTheLensRatherThanAroundIt) {
  // Barrel distortion bends the edge of the frame outward, so the lens sees more of the world than
  // its focal length alone implies. Reading fx and ignoring k1..k3 claims 66 degrees for a frame
  // that actually subtends nearly 73 — an error several times the decentring one the next test is
  // about, in the same function, and it was there while that one was being fixed.
  const Intrinsics lens = Distorted();
  const UnprojectedDirection left = Unproject(lens, Pixel{0.0, lens.cy});
  const UnprojectedDirection right = Unproject(lens, Pixel{960.0, lens.cy});
  ASSERT_TRUE(left.valid);
  ASSERT_TRUE(right.valid);
  const double subtended = AngleBetweenDirections(left.direction, right.direction) * kDegPerRad;
  EXPECT_GT(subtended, 70.0);
  EXPECT_NEAR(HorizontalFovDeg(lens), subtended, 1e-9);
}

TEST(HorizontalFovDeg, ALensWhoseEdgeHasNoPreimageSaysNothingRatherThanAnAngle) {
  // With k1 = -1 the frame's own edge is past the fold: no direction lands there. An angle
  // computed from the focal length would report a confident 66 degrees for a lens whose picture
  // has no left-hand side.
  Intrinsics lens = Phone();
  lens.k1 = -1.0;
  ASSERT_TRUE(IsUsableLens(lens));
  ASSERT_FALSE(Unproject(lens, Pixel{0.0, lens.cy}).valid);
  EXPECT_DOUBLE_EQ(HorizontalFovDeg(lens), 0.0);
}

TEST(HorizontalFovDeg, AnOffCentreLensSeesLessThanACentredOneOfTheSameFocalLength) {
  // The two angles' tangents sum to width/fx however the centre moves, and atan is concave, so a
  // constrained sum of two of them is largest when they are equal — the centred case. Taking twice
  // the angle to a half-width instead, the reading that looks right and is one line shorter,
  // overstates a lens whose centre sits at 300 of 960 by 2.1 degrees.
  //
  // The comparison is against the centred lens. This test was named for a weaker one — "less than
  // twice its wider half", which is 83.5 degrees here and follows from monotonicity alone.
  Intrinsics lens = Phone();
  const double centred = HorizontalFovDeg(lens);
  EXPECT_NEAR(centred, 66.0, 1e-9);

  lens.cx = 300.0;
  const double offCentre = HorizontalFovDeg(lens);
  EXPECT_LT(offCentre, centred);
  EXPECT_NEAR(offCentre, 63.854, 1e-3);

  // Still the whole image, though: the far edge is further away than it was.
  const ProjectedPixel right = Project(lens, OffAxis(41.0, 0));
  ASSERT_TRUE(right.valid);
  EXPECT_GT(right.pixel.x, 900.0);
}

// ------------------------------------------------------------------ which way is up

TEST(Project, TheForwardAxisLandsOnTheOpticalCentre) {
  for (const Intrinsics& lens : {Phone(), Distorted()}) {
    const ProjectedPixel p = Project(lens, Vec3{0, 0, -1});
    ASSERT_TRUE(p.valid);
    // Exactly, even distorted: every Brown-Conrady term is a multiple of the radius, and here it
    // is zero. A model that moved the centre would have a sign error in the tangential terms.
    EXPECT_NEAR(p.pixel.x, lens.cx, 1e-12);
    EXPECT_NEAR(p.pixel.y, lens.cy, 1e-12);
  }
}

TEST(Project, UpInTheWorldIsUpTheImageWhichIsADecreasingY) {
  // The Y axis flips between camera space and raster space, and a consistently upside-down model
  // round-trips perfectly while stitching a sphere that is upside down — so the round-trip tests
  // cannot see it. This one and `TheEdgeOfTheFieldOfViewIsTheEdgeOfTheImage` both can; the comment
  // here claimed to be the only one until a reviewer ran the sabotage and found two failures.
  const Intrinsics lens = Phone();
  const ProjectedPixel up = Project(lens, OffAxis(10.0, 1));
  ASSERT_TRUE(up.valid);
  EXPECT_LT(up.pixel.y, lens.cy);
  EXPECT_NEAR(up.pixel.x, lens.cx, 1e-9);
}

TEST(Project, RightInTheWorldIsRightTheImage) {
  const Intrinsics lens = Phone();
  const ProjectedPixel right = Project(lens, OffAxis(10.0, 0));
  ASSERT_TRUE(right.valid);
  EXPECT_GT(right.pixel.x, lens.cx);
  EXPECT_NEAR(right.pixel.y, lens.cy, 1e-9);
}

TEST(Project, TheEdgeOfTheFieldOfViewIsTheEdgeOfTheImage) {
  // This is what ties the focal length to the angle. If fx were derived from the full width
  // rather than the half width, this lands at one and a half times the image.
  const Intrinsics lens = Phone();
  const ProjectedPixel right = Project(lens, OffAxis(33.0, 0));
  ASSERT_TRUE(right.valid);
  EXPECT_NEAR(right.pixel.x, 960.0, 1e-9);

  const ProjectedPixel top = Project(lens, OffAxis(25.0, 1));
  ASSERT_TRUE(top.valid);
  EXPECT_NEAR(top.pixel.y, 0.0, 1e-9);
}

TEST(Project, EveryDirectionInsideTheFieldOfViewHasAnImageInsideTheFrame) {
  // The completeness invariant, in miniature: a lens that claims 66 by 50 degrees must actually
  // put all of them on the sensor, or a coverage plan tessellated from that claim leaves gaps.
  const Intrinsics lens = Phone();
  // Counted in integers: accumulating `h += 2.0` from -32.9 stops at 31.1 and never tests the far
  // edge at all, which is exactly where a field-of-view error would show. A reviewer found a 100px
  // error confined to 32.2-32.9 degrees passing the whole suite.
  for (int hi = 0; hi <= 34; ++hi) {
    const double h = -32.9 + 65.8 * static_cast<double>(hi) / 34.0;
    for (int vi = 0; vi <= 26; ++vi) {
      const double v = -24.9 + 49.8 * static_cast<double>(vi) / 26.0;
      const double th = h / kDegPerRad, tv = v / kDegPerRad;
      const Vec3 d{std::tan(th), std::tan(tv), -1.0};
      const ProjectedPixel p = Project(lens, d);
      ASSERT_TRUE(p.valid) << h << "," << v;
      EXPECT_GE(p.pixel.x, 0.0) << h << "," << v;
      EXPECT_LE(p.pixel.x, 960.0) << h << "," << v;
      EXPECT_GE(p.pixel.y, 0.0) << h << "," << v;
      EXPECT_LE(p.pixel.y, 1280.0) << h << "," << v;
    }
  }
}

TEST(Project, TheLengthOfTheDirectionIsNotRead) {
  const Intrinsics lens = Distorted();
  const Vec3 d = OffAxis(15.0, 0);
  const ProjectedPixel unit = Project(lens, d);
  const ProjectedPixel long_ = Project(lens, Vec3{d.x * 1e6, d.y * 1e6, d.z * 1e6});
  ASSERT_TRUE(unit.valid);
  ASSERT_TRUE(long_.valid);
  EXPECT_NEAR(unit.pixel.x, long_.pixel.x, 1e-6);
  EXPECT_NEAR(unit.pixel.y, long_.pixel.y, 1e-6);
}

// ------------------------------------------------------------------ having no answer

TEST(Project, ADirectionBehindTheCameraHasNoImage) {
  // Without this the divide by depth silently changes sign and the direction lands, mirrored,
  // somewhere plausible inside the frame.
  const Intrinsics lens = Phone();
  EXPECT_FALSE(Project(lens, Vec3{0, 0, 1}).valid);
  EXPECT_FALSE(Project(lens, OffAxis(179.0, 0)).valid);
}

TEST(Project, ADirectionAlongThePrincipalPlaneHasNoImage) {
  const Intrinsics lens = Phone();
  EXPECT_FALSE(Project(lens, Vec3{1, 0, 0}).valid);
  EXPECT_FALSE(Project(lens, Vec3{0, 1, 0}).valid);
}

TEST(Project, ADirectionThatIsNotAMeasurementHasNoImage) {
  const Intrinsics lens = Phone();
  EXPECT_FALSE(Project(lens, Vec3{kNaN, 0, -1}).valid);
  EXPECT_FALSE(Project(lens, Vec3{0, kInf, -1}).valid);
  EXPECT_FALSE(Project(lens, Vec3{0, 0, 0}).valid);
  // An infinite depth is the one that does not announce itself. The other rows above are caught
  // twice over, because dividing by a finite depth carries the NaN into the normalised
  // coordinates where a second check is waiting; here the division *cures* it — 1 / inf is a
  // perfectly finite zero — and every later test passes. Without the check on the way in, this
  // answers "dead ahead, exactly on the optical centre" for a direction nobody measured.
  EXPECT_FALSE(Project(lens, Vec3{1, 1, -kInf}).valid);
  EXPECT_FALSE(Project(lens, Vec3{1, 1, kInf}).valid);
}

TEST(Project, AnUnusableLensProjectsNothing) {
  EXPECT_FALSE(Project(Intrinsics{}, Vec3{0, 0, -1}).valid);
}

TEST(Project, ARadiusPastTheFirstFoldIsRefusedEvenWhereTheSlopeHasTurnedPositiveAgain) {
  // The fold check is a statement about the whole way out from the optical centre, not about the
  // radius it is handed. `RadialSlope` is a cubic in r^2 and a cubic can dip below zero and come
  // back: with k1 = -1 and k2 = +0.3 it is 1 - 3r^2 + 1.5r^4, negative between r = 0.650 and
  // r = 1.256 and positive on either side. A check that only asks about the endpoint therefore
  // re-admits radii the image has already folded over.
  //
  // Measured on this lens before the fix: 12.65 and 51.93 degrees — 39 degrees apart — projected
  // to pixels 1.4e-06 apart, both `valid`, and `Unproject` answered the near one with confidence.
  // The round-trip check cannot catch that, because both directions really do project there.
  Intrinsics lens = Phone();
  lens.k1 = -1.0;
  lens.k2 = 0.3;
  ASSERT_TRUE(IsUsableLens(lens));
  EXPECT_TRUE(Project(lens, OffAxis(12.65, 0)).valid);
  EXPECT_FALSE(Project(lens, OffAxis(51.93, 0)).valid);
}

TEST(Unproject, ATangentialLensNeverAnswersWithTheOtherPreimage) {
  // p1 and p2 are not radial, so no amount of care about the radial polynomial says anything about
  // them. Before the Jacobian was checked, this lens answered an in-frame pixel with a bearing
  // **86.8 degrees** wrong, and the round-trip check could not object — that bearing really does
  // project there. It is the other preimage, not a failure to converge, which is precisely the
  // class of error a round-trip test is blind to.
  //
  // Stated as the invariant rather than as the one pixel that caught it: nothing this lens accepts
  // may come back as a different direction.
  Intrinsics lens = Distorted();
  lens.p2 = 0.2;
  ASSERT_TRUE(IsUsableLens(lens));

  int accepted = 0;
  for (double h = -60.0; h <= 60.0; h += 1.5) {
    for (double v = -45.0; v <= 45.0; v += 1.5) {
      const Vec3 d = Normalize(Vec3{std::tan(h / kDegPerRad), std::tan(v / kDegPerRad), -1.0});
      const ProjectedPixel p = Project(lens, d);
      if (!p.valid) continue;
      if (p.pixel.x < 0.0 || p.pixel.x > 960.0 || p.pixel.y < 0.0 || p.pixel.y > 1280.0) continue;
      const UnprojectedDirection back = Unproject(lens, p.pixel);
      if (!back.valid) continue;
      ++accepted;
      EXPECT_LT(AngleBetweenDirections(back.direction, d) * kDegPerRad, 1e-3) << h << "," << v;
    }
  }
  // Refusing everything would satisfy the loop above and prove nothing. This lens is strongly but
  // not absurdly distorted and most of its frame is real.
  EXPECT_GT(accepted, 200);
}

TEST(Unproject, APixelThatIsNotAMeasurementYieldsNoDirection) {
  const Intrinsics lens = Phone();
  EXPECT_FALSE(Unproject(lens, Pixel{kNaN, 640}).valid);
  EXPECT_FALSE(Unproject(lens, Pixel{480, kInf}).valid);
}

TEST(Unproject, AnUnusableLensYieldsNoDirection) {
  EXPECT_FALSE(Unproject(Intrinsics{}, Pixel{480, 640}).valid);
}

// ------------------------------------------------------------------ the round trip

TEST(Unproject, TheOpticalCentreLooksStraightAhead) {
  for (const Intrinsics& lens : {Phone(), Distorted()}) {
    const UnprojectedDirection u = Unproject(lens, Pixel{lens.cx, lens.cy});
    ASSERT_TRUE(u.valid);
    EXPECT_NEAR(AngleBetweenDirections(u.direction, Vec3{0, 0, -1}), 0.0, 1e-12);
  }
}

TEST(Unproject, ADirectionSurvivesTheTripToAPixelAndBack) {
  const Intrinsics lens = Phone();
  for (int hi = 0; hi <= 16; ++hi) {
    const double h = -32.0 + 4.0 * static_cast<double>(hi);
    for (int vi = 0; vi <= 12; ++vi) {
      const double v = -24.0 + 4.0 * static_cast<double>(vi);
      const Vec3 d = Normalize(Vec3{std::tan(h / kDegPerRad), std::tan(v / kDegPerRad), -1.0});
      const ProjectedPixel p = Project(lens, d);
      ASSERT_TRUE(p.valid) << h << "," << v;
      const UnprojectedDirection back = Unproject(lens, p.pixel);
      ASSERT_TRUE(back.valid) << h << "," << v;
      // A thousandth of a degree. Registration is asked for a median error well under a degree,
      // so a lens model that loses more than this in a round trip is already the error budget.
      EXPECT_LT(AngleBetweenDirections(back.direction, d) * kDegPerRad, 1e-3) << h << "," << v;
    }
  }
}

TEST(Unproject, TheRoundTripSurvivesDistortion) {
  // The one that matters: Brown-Conrady has no closed-form inverse, so this is the only evidence
  // the iteration converges to the right place rather than merely to a stable one.
  const Intrinsics lens = Distorted();
  for (int hi = 0; hi <= 16; ++hi) {
    const double h = -32.0 + 4.0 * static_cast<double>(hi);
    for (int vi = 0; vi <= 12; ++vi) {
      const double v = -24.0 + 4.0 * static_cast<double>(vi);
      const Vec3 d = Normalize(Vec3{std::tan(h / kDegPerRad), std::tan(v / kDegPerRad), -1.0});
      const ProjectedPixel p = Project(lens, d);
      ASSERT_TRUE(p.valid) << h << "," << v;
      const UnprojectedDirection back = Unproject(lens, p.pixel);
      ASSERT_TRUE(back.valid) << h << "," << v;
      EXPECT_LT(AngleBetweenDirections(back.direction, d) * kDegPerRad, 1e-3) << h << "," << v;
    }
  }
}

TEST(Project, TheDistortionTermsAreOpenCVsInOpenCVsOrder) {
  // The header invites `k1 k2 p1 p2 k3` from a calibration done elsewhere, which is a promise about
  // which coefficient multiplies what. `DistortionIsNotQuietlyIgnored` below cannot check it: a
  // consistent k2/k3 swap moves a 30-degree pixel by 4.4px against a 20px threshold, so the whole
  // suite stays green while the promise is false.
  //
  // So: one point, worked out by hand from the published Brown-Conrady form, as decimal constants
  // rather than as the formula restated — a test that recomputes the implementation cannot disagree
  // with it. At xn = 0.3, yn = 0.2 with k1..k3 = 0.1, 0.02, 0.003 and p1, p2 = 0.05, 0.07:
  //   r2     = 0.13
  //   radial = 1 + k1*r2 + k2*r2^2 + k3*r2^3            = 1.013344591
  //   xd     = xn*radial + 2*p1*xn*yn + p2*(r2 + 2xn^2) = 0.3317033773
  //   yd     = yn*radial + p1*(r2 + 2yn^2) + 2*p2*xn*yn = 0.2215689182
  // A k2/k3 swap moves xd by 7.5e-05 and a p1/p2 swap by 3.8e-03; the tolerance below is 1e-09, so
  // either is caught by four orders of magnitude.
  Intrinsics lens = Phone();
  lens.k1 = 0.1;
  lens.k2 = 0.02;
  lens.k3 = 0.003;
  lens.p1 = 0.05;
  lens.p2 = 0.07;

  // yn is the negated camera-space y, so this direction has xn = 0.3 and yn = 0.2 exactly.
  const ProjectedPixel p = Project(lens, Vec3{0.3, -0.2, -1.0});
  ASSERT_TRUE(p.valid);
  EXPECT_NEAR((p.pixel.x - lens.cx) / lens.fx, 0.3317033773, 1e-9);
  EXPECT_NEAR((p.pixel.y - lens.cy) / lens.fy, 0.2215689182, 1e-9);
}

TEST(Unproject, DistortionIsNotQuietlyIgnored) {
  // Guards the pair of tests above against the reading that makes both trivially pass: a Project
  // and an Unproject that both drop the coefficients round-trip perfectly and are both wrong.
  const ProjectedPixel plain = Project(Phone(), OffAxis(30.0, 0));
  const ProjectedPixel bent = Project(Distorted(), OffAxis(30.0, 0));
  ASSERT_TRUE(plain.valid);
  ASSERT_TRUE(bent.valid);
  EXPECT_GT(std::abs(plain.pixel.x - bent.pixel.x), 20.0);
}

// ---------------------------------------------------- a lens this model cannot describe

TEST(Project, ADistortionThatFoldsTheImageBackOnItselfIsRefusedRatherThanGuessed) {
  // With k1 = -1 the radial map r -> r(1 - r^2) stops increasing at r = 1/sqrt(3) ~= 0.5774 and
  // then runs backwards, so beyond that two directions share a pixel and no inverse exists.
  // tan(20 deg) = 0.364 is inside it; tan(33 deg) = 0.649 is not.
  Intrinsics lens = Phone();
  lens.k1 = -1.0;
  ASSERT_TRUE(IsUsableLens(lens));
  EXPECT_TRUE(Project(lens, OffAxis(20.0, 0)).valid);
  EXPECT_FALSE(Project(lens, OffAxis(33.0, 0)).valid);
}

TEST(Unproject, APixelTheIterationNeverReachesIsRefusedRatherThanAnswered) {
  // A pixel the iteration never arrives at. At (1010, 260) on this lens it is still moving when the
  // budget runs out, and where it stops projects to a perfectly valid pixel 246 pixels away from the
  // one it was asked about. Only comparing the round trip against the input can tell.
  //
  // Note what this is *not*. A settled fixed point satisfies the forward equation by construction,
  // so it is always a genuine preimage — "converged on a non-preimage" is not a state this
  // iteration can be in, and a reviewer's 223,608-sample sweep found zero of them. What the
  // tolerance catches is exhaustion. Landing on the *wrong* preimage of two is a different failure
  // that this check is structurally blind to, and the fold test is what catches that one.
  //
  // The earlier version of this test used a pixel that merely converged *slowly*: 835 passes to a
  // residual of 6.7e-12, refused only because the budget was 500. A reviewer caught that it was
  // recording a truncation as though it were a property of the lens — and refusing a well-defined
  // pixel is what this file's own ADR calls the defect. The budget is now 5000 and that pixel is
  // accepted, as it always should have been.
  Intrinsics lens = Phone();
  lens.k1 = -0.9;
  lens.p2 = 0.6;
  ASSERT_TRUE(IsUsableLens(lens));
  EXPECT_FALSE(Unproject(lens, Pixel{1010.0, 260.0}).valid);

  // The slow one, for contrast: same lens, and it now answers.
  EXPECT_TRUE(Unproject(lens, Pixel{lens.cx + 0.38 * lens.fx, lens.cy + 0.30 * lens.fy}).valid);
}

TEST(Unproject, TheTopLeftPixelIsNotAnsweredByTheThingThatMeansRefusal) {
  // The one guard in this file that carries weight of its own rather than backstopping a later one,
  // and it took a sweep to find an input that shows it. A refused `ProjectedPixel` carries the pixel
  // `(0, 0)`. `Unproject` verifies its answer by projecting it and comparing to the pixel it was
  // handed — so for an input of exactly `(0, 0)`, and for no other input, a refusal compares equal
  // to the question and the tolerance test waves it through.
  //
  // This lens is one of 11,867 in a swept grid where the iteration settles on a point `Project`
  // refuses. Without the `check.valid` line it answers the image's top-left corner with a confident
  // direction that nothing looks in.
  Intrinsics lens = Phone();
  lens.k1 = -0.9;
  lens.k2 = -0.6;
  lens.p1 = -0.3;
  lens.p2 = -0.35;
  ASSERT_TRUE(IsUsableLens(lens));
  EXPECT_FALSE(Unproject(lens, Pixel{0.0, 0.0}).valid);
}

TEST(Unproject, ASlowInverseIsIteratedToTheEndRatherThanGivenUpOn) {
  // A pixel that exists, has exactly one preimage, and takes a long time to find it. With
  // k1 = -0.9 the fold sits at r = 0.609, so a direction at r = 0.55 is comfortably inside it —
  // but the fixed point converges linearly with a ratio approaching 1 near the fold, so a small
  // iteration budget lands short and the convergence check then refuses a perfectly well defined
  // pixel. Measured on this lens: 20 iterations reach 89.6% of the invertible radius, 100 reach
  // 99.6%, 500 reach 99.98%. This direction sits in the band the first of those refuses.
  Intrinsics lens = Phone();
  lens.k1 = -0.9;
  const double theta = std::atan(0.55);
  const Vec3 d{std::sin(theta), 0, -std::cos(theta)};
  const ProjectedPixel p = Project(lens, d);
  ASSERT_TRUE(p.valid);
  const UnprojectedDirection back = Unproject(lens, p.pixel);
  ASSERT_TRUE(back.valid);
  EXPECT_LT(AngleBetweenDirections(back.direction, d) * kDegPerRad, 1e-3);
}

TEST(Unproject, APixelBeyondTheFoldIsRefusedRatherThanApproximated) {
  // The matching refusal on the way back, at the boundary rather than far outside it. With k1 = -1
  // the fold sits at r = 0.5774 and the largest *pixel* radius that still has a preimage is
  // 0.5774 * (1 - 1/3) = 0.3849, i.e. x = cx + 0.3849 * fx = 764.49. Just inside answers; just
  // outside must not.
  //
  // The earlier version of this test used x = 5000, which is refused on the first pass because the
  // radial polynomial has already gone negative there — nothing to do with the fold the comment
  // described. A reviewer pointed out it stayed green with the fold reasoning deleted entirely.
  Intrinsics lens = Phone();
  lens.k1 = -1.0;
  const ProjectedPixel inside = Project(lens, OffAxis(20.0, 0));
  ASSERT_TRUE(inside.valid);
  EXPECT_TRUE(Unproject(lens, inside.pixel).valid);

  EXPECT_TRUE(Unproject(lens, Pixel{764.0, lens.cy}).valid);
  EXPECT_FALSE(Unproject(lens, Pixel{765.0, lens.cy}).valid);
  EXPECT_FALSE(Unproject(lens, Pixel{5000.0, lens.cy}).valid);
}

}  // namespace
}  // namespace sphanorama
