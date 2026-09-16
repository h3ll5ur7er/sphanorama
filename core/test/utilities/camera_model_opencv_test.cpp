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

#include <algorithm>
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


// The rotation that best carries `from` onto `to`, both unit directions, by Kabsch.
//
// OpenCV's SVD rather than a hand-rolled one: a three-by-three eigen decomposition written here
// would be a second answer to a question this repository already has a dependency for, and it is
// the step whose sign convention is easy to get wrong. The reflection guard is the whole of that
// convention — without it a degenerate correspondence set returns an improper rotation with
// determinant -1, which scores as a perfect fit and is not a rotation.
cv::Matx33d BestRotation(const std::vector<cv::Vec3d>& from, const std::vector<cv::Vec3d>& to) {
  cv::Matx33d covariance = cv::Matx33d::zeros();
  for (size_t at = 0; at < from.size(); ++at) {
    covariance += cv::Matx33d(to[at][0] * from[at][0], to[at][0] * from[at][1], to[at][0] * from[at][2],
                              to[at][1] * from[at][0], to[at][1] * from[at][1], to[at][1] * from[at][2],
                              to[at][2] * from[at][0], to[at][2] * from[at][1], to[at][2] * from[at][2]);
  }
  cv::Matx33d u, vt;
  cv::Matx31d w;
  cv::SVD::compute(covariance, w, u, vt);
  cv::Matx33d flip = cv::Matx33d::eye();
  flip(2, 2) = cv::determinant(u * vt) < 0 ? -1.0 : 1.0;
  return u * flip * vt;
}

double AngleBetweenDeg(const cv::Matx33d& a, const cv::Matx33d& b) {
  const cv::Matx33d between = a.t() * b;
  const double trace = between(0, 0) + between(1, 1) + between(2, 2);
  return std::acos(std::clamp((trace - 1.0) / 2.0, -1.0, 1.0)) * kDegPerRad;
}

cv::Matx33d TurnAboutY(double degrees) {
  const double t = degrees / kDegPerRad;
  return cv::Matx33d(std::cos(t), 0, std::sin(t), 0, 1, 0, -std::sin(t), 0, std::cos(t));
}

cv::Vec3d ToCv(const Vec3& v) { return cv::Vec3d(v.x, v.y, v.z); }

/**
 * ADR 0061's shift table, asserted rather than left as prose.
 *
 * **Why this test exists is a finding rather than a plan.** The ADR declined ADR 0060's rule —
 * assert a published figure from a test — on the ground that an ADR records what was believed at
 * the time and a test pinning it would be asserting history. A reviewer answered that the same is
 * true of everything 0060 pinned, and that a failing test is precisely the *trigger* to supersede
 * an ADR rather than a demand to edit one. The argument landed because of what happened next: the
 * ADR's first table was wrong, the retraction that replaced it was measured on a lens with square
 * pixels, and the accuracy dataset's lens has none — `fy` is 514.68 against `fx`'s 492.76, because
 * `LensFromFieldOfView` solves the two axes separately from two different angles. Nothing could
 * fail on either mistake. This is the mechanism that would have caught both.
 *
 * **What is measured.** Every pixel centre of a 640x480 frame whose image, after a 30-degree turn
 * about +Y, lands inside the frame — 160,000 of 307,200 — unprojected through the engine's
 * convention shifted by `s` and fitted back by Kabsch. `ReadBearings` hands OpenCV keypoint
 * coordinates to `Unproject` unchanged, so a feature at the centre of pixel `i` arrives as `i`
 * where the model wants `i + 0.5`: the engine sits at `s = 0` and the correction is `s = +0.5`.
 *
 * **Cross-checked against a numpy reimplementation of the same geometry, which agrees to 1e-6 —
 * after that reimplementation was corrected.** It first built directions as `(x, y, +1)` where
 * `camera_model.cpp:431` builds `(x, -y, -1)`, and the mirror-image convention moved the table by
 * 0.2% at every row while leaving the linearity and the exact zero intact. So the structure of this
 * measurement survives getting the handedness wrong and the value does not, which is the argument
 * for pinning it here, through the model the engine actually calls, rather than beside it.
 *
 * Linear in the shift and exactly zero at `+0.5`, which is the claim that matters — the gap is a
 * constant image-plane translation and correcting it is geometrically exact. The absolute figure
 * is what the ADR publishes and what the roadmap's prose compares against SIFT's median.
 *
 * This lens rather than a convenient one: it is the accuracy dataset's, built by the same two
 * angles `registration_accuracy_test.cpp` renders through, so the number is about the measurement
 * that is published and not about a lens chosen to make the arithmetic tidy.
 */
TEST(CameraModelAgainstOpenCV, TheHalfPixelShiftCostsTheAngleADR0061Publishes) {
  const Intrinsics lens = LensFromFieldOfView(66.0, 50.0, 640, 480);
  // Guarded, because the whole point of using the dataset's own lens is that it is not square, and
  // a future change to either angle would quietly re-measure the table this test is pinning.
  ASSERT_NEAR(lens.fx, 492.756788, 1e-6);
  ASSERT_NEAR(lens.fy, 514.681661, 1e-6);

  const cv::Matx33d turn = TurnAboutY(30.0);

  // The correspondences, as pixel coordinates under the model's own corner convention. Built once:
  // the shift is applied to these, so every row of the table is fitted to the same set and the
  // sweep is a property of the shift rather than of five different samplings.
  std::vector<Pixel> inA, inB;
  for (int32_t y = 0; y < lens.height; ++y) {
    for (int32_t x = 0; x < lens.width; ++x) {
      const Pixel centre{static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5};
      const UnprojectedDirection ray = Unproject(lens, centre);
      if (!ray.valid) continue;
      // The same world point in the other frame. `turn` is applied to the direction, so this is
      // where the second camera sees what the first saw at `centre`.
      const cv::Vec3d turned = turn.t() * ToCv(ray.direction);
      const ProjectedPixel image = Project(lens, Vec3{turned[0], turned[1], turned[2]});
      if (!image.valid) continue;
      if (image.pixel.x < 0 || image.pixel.x > lens.width) continue;
      if (image.pixel.y < 0 || image.pixel.y > lens.height) continue;
      inA.push_back(centre);
      inB.push_back(image.pixel);
    }
  }
  ASSERT_EQ(inA.size(), 160000u) << "the overlap is not the one the ADR's figures were measured on";

  struct Row { double shift; double expectedDeg; };
  // ADR 0061's table, to the digit. A tolerance of 1e-5 degrees is four orders below the smallest
  // non-zero row, so a row that moved at all would fail rather than round into agreement — which is
  // the failure mode ADR 0060 records, a bound loose enough to be satisfied by the wrong number.
  const Row rows[] = {{-0.50, 0.021008}, {-0.25, 0.015752}, {0.00, 0.010498},
                      {0.25, 0.005248},  {0.50, 0.000000}};

  for (const Row& row : rows) {
    std::vector<cv::Vec3d> a, b;
    a.reserve(inA.size());
    b.reserve(inB.size());
    for (size_t at = 0; at < inA.size(); ++at) {
      const UnprojectedDirection ra =
          Unproject(lens, Pixel{inA[at].x - 0.5 + row.shift, inA[at].y - 0.5 + row.shift});
      const UnprojectedDirection rb =
          Unproject(lens, Pixel{inB[at].x - 0.5 + row.shift, inB[at].y - 0.5 + row.shift});
      ASSERT_TRUE(ra.valid && rb.valid) << "a shifted pixel of a pinhole lens has no ray";
      a.push_back(ToCv(ra.direction));
      b.push_back(ToCv(rb.direction));
    }
    // `b` holds `turn.t() * a` by construction, so the rotation that carries **b onto a** is
    // `turn` itself. Written this way round rather than comparing against `turn.t()`, because the
    // quantity the engine returns is the one that carries the second frame's bearings onto the
    // first — `PairwiseResult::relativeRotation` is `Conjugate(q[b]) * q[a]` — and a test fitting
    // the inverse would be measuring the same geometry against a different claim. The first draft
    // had the arguments the other way and failed by exactly 60 degrees, which is 30 twice: a fit
    // that is wrong by the turn rather than by the shift.
    const double error = AngleBetweenDeg(BestRotation(b, a), turn);
    EXPECT_NEAR(error, row.expectedDeg, 1e-5)
        << "shift " << row.shift << " fits the turn " << error << " degrees out";
  }
}

/**
 * And what the same offset does to one bearing, which is a different number and was published as a
 * third one.
 *
 * `docs/06-roadmap.md` quoted 0.0581 degrees, which is `atan(0.5 / fx)` — the displacement along
 * *one* axis. The offset is `(-0.5, -0.5)`, so both axes move, and on a lens whose axes differ the
 * two do not even move by the same angle. 0.0805 degrees at the optical centre is the real figure,
 * and it falls toward the corners because a pixel out there subtends less angle.
 *
 * Asserted beside the table above because the two are constantly confused and the confusion is
 * load-bearing: the per-bearing figure is larger than SIFT's median and the per-fit one is half of
 * it, so which number a reader picks up decides whether the gap looks like the dominant error or a
 * minor one. It is neither — it is 7.7 times the error it leaves in a fitted rotation, and the fit
 * is what absorbs the difference.
 */
TEST(CameraModelAgainstOpenCV, TheHalfPixelOffsetMovesABearingFurtherThanItMovesAFit) {
  const Intrinsics lens = LensFromFieldOfView(66.0, 50.0, 640, 480);

  const auto displacementDeg = [&lens](const Pixel& centre) {
    const UnprojectedDirection truth = Unproject(lens, centre);
    const UnprojectedDirection read = Unproject(lens, Pixel{centre.x - 0.5, centre.y - 0.5});
    EXPECT_TRUE(truth.valid && read.valid);
    return std::acos(std::clamp(Dot(truth.direction, read.direction), -1.0, 1.0)) * kDegPerRad;
  };

  EXPECT_NEAR(displacementDeg(Pixel{lens.cx, lens.cy}), 0.0805, 1e-4);

  double smallest = 180.0, largest = 0.0;
  for (int32_t y = 0; y < lens.height; ++y) {
    for (int32_t x = 0; x < lens.width; ++x) {
      const double moved =
          displacementDeg(Pixel{static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5});
      smallest = std::min(smallest, moved);
      largest = std::max(largest, moved);
    }
  }
  // Largest at the centre, smallest at a corner — the opposite of a radial distortion, which is the
  // sign that this is a translation in the image plane rather than a lens term.
  EXPECT_NEAR(largest, 0.0805, 1e-4);
  EXPECT_NEAR(smallest, 0.0494, 1e-4);
  // One axis is not the answer, and this is the figure that was published as though it were.
  EXPECT_NEAR(std::atan(0.5 / lens.fx) * kDegPerRad, 0.0581, 1e-4);
  EXPECT_GT(largest, std::atan(0.5 / lens.fx) * kDegPerRad);
}

}  // namespace
}  // namespace sphanorama
