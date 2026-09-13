// Our camera model against OpenCV's, which is the point of depending on OpenCV at all.
//
// Three review rounds argued about whether the header's claim — "Brown-Conrady in OpenCV's
// parameter convention, so `k1 k2 p1 p2 k3` from a calibration elsewhere can be dropped into
// `Intrinsics` unchanged" — is true. One lens verified the terms by hand and another found nothing
// pinned them. `Project.TheDistortionTermsAreOpenCVsInOpenCVsOrder` pins them against constants a
// human worked out. This pins them against the implementation the claim actually names.
//
// The two tests below are deliberately different in kind:
//
//   - The forward one is an *agreement* test. `cv::projectPoints` is closed-form, so any difference
//     is a difference in the model rather than in a solver, and the tolerance is tight enough that
//     a swapped coefficient could not hide in it.
//   - The inverse one is not an agreement test, because OpenCV's `undistortPoints` runs five passes
//     of the very fixed-point iteration this file replaced in round three, and on a wide lens it is
//     the one that is wrong. So instead our `Unproject` is asked to invert *their* forward map. That
//     is a stronger statement than agreeing with their inverse, and it is the one that would have
//     caught the round-three defect had it existed then.
#include <gtest/gtest.h>

#include <opencv2/calib3d.hpp>
#include <opencv2/core.hpp>

#include <cmath>
#include <numbers>
#include <vector>

#include "utilities/camera_model.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr double kDegPerRad = 180.0 / std::numbers::pi;

Intrinsics Phone() { return LensFromFieldOfView(66.0, 50.0, 960, 1280); }

Intrinsics Distorted() {
  Intrinsics lens = Phone();
  lens.k1 = -0.28;
  lens.k2 = 0.12;
  lens.k3 = -0.02;
  lens.p1 = 0.001;
  lens.p2 = -0.0015;
  return lens;
}

cv::Mat CameraMatrix(const Intrinsics& lens) {
  return (cv::Mat_<double>(3, 3) << lens.fx, 0, lens.cx, 0, lens.fy, lens.cy, 0, 0, 1);
}

// OpenCV's order, which is the order the header promises `Intrinsics` accepts: k1 k2 p1 p2 k3.
cv::Mat DistCoeffs(const Intrinsics& lens) {
  return (cv::Mat_<double>(1, 5) << lens.k1, lens.k2, lens.p1, lens.p2, lens.k3);
}

// Camera space here is -Z forward and +Y up; OpenCV's is +Z forward and +Y down. This is the whole
// of the conversion, and getting it wrong is the failure the tests below exist to catch.
cv::Point3d ToOpenCV(const Vec3& d) { return cv::Point3d(d.x, -d.y, -d.z); }

std::vector<cv::Point2d> ProjectWithOpenCV(const Intrinsics& lens,
                                           const std::vector<cv::Point3d>& points) {
  std::vector<cv::Point2d> pixels;
  const cv::Mat zero = cv::Mat::zeros(3, 1, CV_64F);
  cv::projectPoints(points, zero, zero, CameraMatrix(lens), DistCoeffs(lens), pixels);
  return pixels;
}

TEST(CameraModelAgainstOpenCV, ProjectAgreesWithProjectPoints) {
  for (const Intrinsics& lens : {Phone(), Distorted()}) {
    std::vector<cv::Point3d> points;
    std::vector<Vec3> directions;
    for (int hi = 0; hi <= 16; ++hi) {
      const double h = -32.0 + 4.0 * static_cast<double>(hi);
      for (int vi = 0; vi <= 12; ++vi) {
        const double v = -24.0 + 4.0 * static_cast<double>(vi);
        const Vec3 d{std::tan(h / kDegPerRad), std::tan(v / kDegPerRad), -1.0};
        directions.push_back(d);
        points.push_back(ToOpenCV(d));
      }
    }
    const std::vector<cv::Point2d> theirs = ProjectWithOpenCV(lens, points);
    ASSERT_EQ(theirs.size(), directions.size());

    for (size_t i = 0; i < directions.size(); ++i) {
      const ProjectedPixel ours = Project(lens, directions[i]);
      ASSERT_TRUE(ours.valid) << i;
      // A ten-thousandth of a pixel. Both evaluate the same polynomial in double precision, so the
      // only thing this leaves room for is the order of the arithmetic, and it leaves no room at
      // all for a swapped or misplaced coefficient — the smallest of those measured 4.4 px.
      EXPECT_NEAR(ours.pixel.x, theirs[i].x, 1e-4) << i;
      EXPECT_NEAR(ours.pixel.y, theirs[i].y, 1e-4) << i;
    }
  }
}

TEST(CameraModelAgainstOpenCV, UnprojectInvertsOpenCVsForwardMap) {
  // Not "agrees with `cv::undistortPoints`" — that runs five passes of a fixed-point iteration and
  // is the thing round three replaced. Inverting their forward map is the stronger claim, and the
  // one that stays true on lenses where their inverse does not converge.
  for (const Intrinsics& lens : {Phone(), Distorted()}) {
    std::vector<cv::Point3d> points;
    std::vector<Vec3> directions;
    for (int hi = 0; hi <= 12; ++hi) {
      const double h = -30.0 + 5.0 * static_cast<double>(hi);
      for (int vi = 0; vi <= 8; ++vi) {
        const double v = -22.0 + 5.5 * static_cast<double>(vi);
        const Vec3 d = Normalize(Vec3{std::tan(h / kDegPerRad), std::tan(v / kDegPerRad), -1.0});
        directions.push_back(d);
        points.push_back(ToOpenCV(d));
      }
    }
    const std::vector<cv::Point2d> theirs = ProjectWithOpenCV(lens, points);

    for (size_t i = 0; i < directions.size(); ++i) {
      const UnprojectedDirection back = Unproject(lens, Pixel{theirs[i].x, theirs[i].y});
      ASSERT_TRUE(back.valid) << i;
      EXPECT_LT(AngleBetweenDirections(back.direction, directions[i]) * kDegPerRad, 1e-6) << i;
    }
  }
}

TEST(CameraModelAgainstOpenCV, TheWideLensOpenCVsOwnInverseCannotSolveIsSolvedHere) {
  // The lens round three was rebuilt around: 115 degrees, k1 = -0.3, k2 = 0.1, folding nowhere. Its
  // forward map is closed-form and OpenCV computes it happily; its inverse is where five passes of
  // a fixed point are not enough, which is exactly what our own solver used to be.
  //
  // This is the cross-check earning its keep. It says our inverse is right about a lens whose
  // forward map an independent implementation agrees on, and it would have failed loudly in round
  // one and round two.
  Intrinsics lens = LensFromFieldOfView(115.0, 90.0, 960, 1280);
  lens.k1 = -0.3;
  lens.k2 = 0.1;
  ASSERT_TRUE(IsUsableLens(lens));

  const Vec3 edge = Normalize(Vec3{-1.669160308753443, 0.0, -1.0});
  const std::vector<cv::Point2d> theirs = ProjectWithOpenCV(lens, {ToOpenCV(edge)});
  ASSERT_EQ(theirs.size(), 1u);
  // OpenCV puts this direction on the frame's left edge, so the pixel is real and in bounds.
  EXPECT_NEAR(theirs[0].x, 0.0, 1e-6);

  const UnprojectedDirection back = Unproject(lens, Pixel{theirs[0].x, theirs[0].y});
  ASSERT_TRUE(back.valid);
  EXPECT_LT(AngleBetweenDirections(back.direction, edge) * kDegPerRad, 1e-6);
}

}  // namespace
}  // namespace sphanorama
