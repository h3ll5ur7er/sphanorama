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

TEST(HorizontalFovDeg, AnOffCentreLensSeesLessThanTwiceItsWiderHalf) {
  // The two half-angles are unequal and atan is concave, so their sum is largest when the optical
  // centre is in the middle. Taking twice the angle to a half-width instead — the reading that
  // looks right and is one line shorter — overstates a lens whose centre sits at 300 of 960 by
  // 2.1 degrees, and the coverage planner would tessellate a sphere with gaps in it accordingly.
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
  // The Y axis flips between camera space and raster space. Nothing else in this file would fail
  // if it did not: a consistently upside-down model round-trips perfectly and stitches a sphere
  // that is upside down.
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
  for (double h = -32.9; h <= 32.9; h += 2.0) {
    for (double v = -24.9; v <= 24.9; v += 2.0) {
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
  for (double h = -32.0; h <= 32.0; h += 4.0) {
    for (double v = -24.0; v <= 24.0; v += 4.0) {
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
  for (double h = -32.0; h <= 32.0; h += 4.0) {
    for (double v = -24.0; v <= 24.0; v += 4.0) {
      const Vec3 d = Normalize(Vec3{std::tan(h / kDegPerRad), std::tan(v / kDegPerRad), -1.0});
      const ProjectedPixel p = Project(lens, d);
      ASSERT_TRUE(p.valid) << h << "," << v;
      const UnprojectedDirection back = Unproject(lens, p.pixel);
      ASSERT_TRUE(back.valid) << h << "," << v;
      EXPECT_LT(AngleBetweenDirections(back.direction, d) * kDegPerRad, 1e-3) << h << "," << v;
    }
  }
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

TEST(Unproject, APixelTheIterationCannotAccountForIsRefusedRatherThanAnswered) {
  // Strong *tangential* distortion is what breaks the reasoning that the radial guard is enough.
  // The radial terms are the ones the fold check reasons about; p1 and p2 are not radial, and with
  // them the fixed point can settle somewhere that is not a preimage of this pixel at all. Radial
  // stays positive the whole way, the point it settles on is inside the fold, and projecting it
  // gives a perfectly valid pixel — just not this one. Nothing but comparing the round trip
  // against the pixel we were handed can tell the difference.
  //
  // Found by sweeping the model rather than by reading it: with the check removed this returns a
  // confident direction, and no other test in this file notices.
  Intrinsics lens = Phone();
  lens.k1 = -0.9;
  lens.p2 = 0.6;
  const UnprojectedDirection u =
      Unproject(lens, Pixel{lens.cx + 0.38 * lens.fx, lens.cy + 0.30 * lens.fy});
  EXPECT_FALSE(u.valid);
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
  // The matching refusal on the way back. The iteration will happily converge somewhere here;
  // answering with it would put a feature at a bearing nothing later could attribute to the lens.
  Intrinsics lens = Phone();
  lens.k1 = -1.0;
  const ProjectedPixel inside = Project(lens, OffAxis(20.0, 0));
  ASSERT_TRUE(inside.valid);
  EXPECT_TRUE(Unproject(lens, inside.pixel).valid);
  // Far outside the invertible radius: fx * 0.649 + cx is where 33 degrees would have landed
  // undistorted, and the fold means nothing projects there.
  EXPECT_FALSE(Unproject(lens, Pixel{5000.0, lens.cy}).valid);
}

}  // namespace
}  // namespace sphanorama
