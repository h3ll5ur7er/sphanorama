// Our camera model against OpenCV's, which is the point of depending on OpenCV at all.
//
// Three review rounds argued about whether the header's claim — "Brown-Conrady in OpenCV's
// parameter convention, so `k1 k2 p1 p2 k3` from a calibration elsewhere can be dropped into
// `Intrinsics` unchanged" — is true. One lens verified the terms by hand and another found nothing
// pinned them. `Project.TheDistortionTermsAreOpenCVsInOpenCVsOrder` pins them against constants a
// human worked out. This pins them against the implementation the claim actually names.
//
// The agreement tests below are deliberately different in kind:
//
//   - The forward one is an *agreement* test. `cv::projectPoints` is closed-form, so any difference
//     is a difference in the model rather than in a solver, and the tolerance is tight enough that
//     a swapped coefficient could not hide in it.
//   - The inverse one is not an agreement test, because OpenCV's `undistortPoints` runs five passes
//     of the very fixed-point iteration this file replaced in round three, and on a wide lens it is
//     the one that is wrong. So instead our `Unproject` is asked to invert *their* forward map. That
//     is a stronger statement than agreeing with their inverse, and it is the one that would have
//     caught the round-three defect had it existed then.
//
// A third test joined them later and is neither: `TheHalfPixelShiftCostsTheAngleADR0061Publishes`
// uses OpenCV for its SVD and asks nothing of OpenCV's camera model. It is here because that is
// where the SVD is, and its own docblock says what it does and does not check. Its companion —
// which needs no OpenCV at all — lives in `camera_model_test.cpp` for the same reason.
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
// would be a second answer to a question this repository already has a dependency for.
//
// **This is the same arithmetic as production's `KabschRotation`, and the `+0.50` row is what keeps
// that from mattering.** A reviewer is right that a test fitting with a copy of the engine's own
// fit cannot tell you the fit is correct — that is ADR 0050's argument, one layer up. What rescues
// it here is that the correspondences at `+0.5` are *exact*: a wrong Kabsch does not recover the
// turn to 1e-5 degrees from exact input, so the zero row is a check on the fit and the other four
// rows are then a measurement of the shift. A shared error would move the zero row too.
//
// Two things production has that this does not, and both are deliberate. `BearingsSpanAPlane`,
// which refuses a degenerate set: the set here is 160,000 correspondences spanning a frame, and a
// test that silently tolerated a degenerate one would have nothing to measure anyway. And the
// reflection guard — `flip(2, 2) = determinant < 0 ? -1 : 1` — which was here and is gone, because
// it cannot fire on this input and a reviewer proved it: replacing it with a constant `1.0` leaves
// every row green. An untested guard kept because it feels safer is the thing the engineering skill
// names; the general-purpose version of this function is `KabschRotation` and it has the guard.
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
  return u * vt;
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
 * `camera_model.cpp` builds `(x, -y, -1)`. That is a y flip *and* a z flip together, and it moved
 * the table by 0.209% at `-0.50` down to 0.052% at `+0.25`, leaving the linearity and the exact
 * zero intact — so a second implementation can get the handedness wrong and still produce a table
 * with the right shape. In absolute terms the two lower rows move by less than the 1e-5 tolerance
 * here, so this test would catch that mutation on three rows of five.
 *
 * **A mutation applied to the model itself it would not catch at all.** Mutating `Project` and
 * `Unproject` together to put image-up at camera-up leaves every row of this table unchanged — a
 * reviewer did it, and the two OpenCV agreement tests above failed while this one passed. The
 * reason is structural rather than a choice of axis, and the body says why.
 * `Unproject.ImageYRunsDownAndCameraYRunsUp` is what asserts the handedness.
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

  // The correspondences, as pixel coordinates under the model's own corner convention. Built once
  // per turn: the shift is applied to these, so every row of a table is fitted to the same set and
  // a sweep is a property of the shift rather than of five different samplings.
  const auto correspond = [&lens](const cv::Matx33d& about, std::vector<Pixel>* a,
                                  std::vector<Pixel>* b) {
    for (int32_t y = 0; y < lens.height; ++y) {
      for (int32_t x = 0; x < lens.width; ++x) {
        const Pixel centre{static_cast<double>(x) + 0.5, static_cast<double>(y) + 0.5};
        const UnprojectedDirection ray = Unproject(lens, centre);
        if (!ray.valid) continue;
        // The same world point in the other frame, so this is where the second camera sees what
        // the first saw at `centre`.
        const cv::Vec3d turned = about.t() * ToCv(ray.direction);
        const ProjectedPixel image = Project(lens, Vec3{turned[0], turned[1], turned[2]});
        if (!image.valid) continue;
        if (image.pixel.x < 0 || image.pixel.x > lens.width) continue;
        if (image.pixel.y < 0 || image.pixel.y > lens.height) continue;
        a->push_back(centre);
        b->push_back(image.pixel);
      }
    }
  };

  const auto fitErrorDeg = [&lens](const std::vector<Pixel>& a, const std::vector<Pixel>& b,
                                   const cv::Matx33d& about, double shift) {
    std::vector<cv::Vec3d> from, to;
    from.reserve(a.size());
    to.reserve(b.size());
    for (size_t at = 0; at < a.size(); ++at) {
      const UnprojectedDirection ra = Unproject(lens, Pixel{a[at].x - 0.5 + shift,
                                                            a[at].y - 0.5 + shift});
      const UnprojectedDirection rb = Unproject(lens, Pixel{b[at].x - 0.5 + shift,
                                                            b[at].y - 0.5 + shift});
      if (!ra.valid || !rb.valid) return -1.0;
      from.push_back(ToCv(ra.direction));
      to.push_back(ToCv(rb.direction));
    }
    return AngleBetweenDeg(BestRotation(to, from), about);
  };

  std::vector<Pixel> inA, inB;
  correspond(turn, &inA, &inB);
  // **This guards `fx`, the width and the turn — not `fy`.** The turn is about `+Y`, so the
  // vertical extent of the overlap is the whole frame and the two `image.pixel.y` bounds reject
  // nothing: setting `fy` to `fx`, or the vertical field of view to 66 degrees, leaves the count at
  // exactly 160,000. It moves for `fx` — 159,150 at `fx x 1.01` and 160,844 at `x 0.99` — and for
  // a half-pixel shift in `cx` (159,960; 159,962 if `cy` moves with it). The one mistake this ADR's
  // table has actually made is a square lens, and it walks straight past this line — which is why
  // `fy` is asserted by name above.
  //
  // An earlier version of this comment said `156,164 at a one per cent change`. That count is real
  // and it is `fx x 1.044`, a four per cent change — so the guard was being sold as four times
  // tighter on the one axis it does guard.
  ASSERT_EQ(inA.size(), 160000u) << "the overlap is not the one the ADR's figures were measured on";

  struct Row { double shift; double expectedDeg; };
  // ADR 0061's table, to the digit, at a tolerance of 1e-5 degrees.
  //
  // **That is sized against the rows and not against every error it has to reject, and the
  // difference is measured rather than assumed.** Against a square lens — the mistake this table
  // has actually made — the `+0.00` row lands at 0.010395, ten times the tolerance out. Against a
  // reversed turn it lands 1.1e-5 out, which clears 1e-5 by ten per cent rather than by the four
  // orders an earlier version of this comment claimed; and the `+0.25` and `+0.50` rows do not
  // catch that one at all, because the thing being perturbed scales with the row. So the rows are
  // not five independent checks of equal strength: the two large-shift rows carry the sensitivity
  // and the two small ones are close to free.
  //
  // Not tightened, because the figures are published to six places and a tolerance below the sixth
  // would be asserting the platform's `acos` rather than the geometry.
  const Row rows[] = {{-0.50, 0.021008}, {-0.25, 0.015752}, {0.00, 0.010498},
                      {0.25, 0.005248},  {0.50, 0.000000}};

  for (const Row& row : rows) {
    // `b` holds `turn.t() * a` by construction, so the rotation that carries **b onto a** is
    // `turn` itself, and `fitErrorDeg` passes them in that order to recover it rather than its
    // inverse. The first draft had them the other way and failed by exactly 60 degrees, which is 30
    // twice — a fit wrong by the whole turn rather than by the shift.
    //
    // **Which of the two the engine returns does not matter here, and a previous version of this
    // comment claimed it did and got it backwards.** `AngleBetweenDeg(R, turn)` equals
    // `AngleBetweenDeg(R.t(), turn.t())`, so the table is the same either way round. For the
    // record, since the wrong version is on a review thread: the engine calls
    // `KabschRotation(from = frame A's bearings, to = frame B's)`, which carries the **first**
    // frame onto the second, and `PairwiseResult::relativeRotation` is `Conjugate(q[b]) * q[a]`,
    // which is what `registration_accuracy_test.cpp`'s `Chain` consumes.
    const double error = fitErrorDeg(inA, inB, turn, row.shift);
    ASSERT_GE(error, 0.0) << "a shifted pixel of a pinhole lens has no ray";
    EXPECT_NEAR(error, row.expectedDeg, 1e-5)
        << "shift " << row.shift << " fits the turn " << error << " degrees out";
  }

  // **What this test cannot do, stated because a draft of it claimed otherwise.** The
  // correspondences are built by unprojecting a pixel, turning the direction, and projecting it
  // back — all through the model under test — so a mutation applied consistently to `Project` and
  // `Unproject` cancels exactly. Write `b = Unproject(Project(M * turn.t() * Unproject(pa)))` for
  // any invertible `M` the model is mutated by and the `M`s meet in the middle: `b` comes out as
  // `turn.t() * a` whatever `M` was, and every row is unchanged.
  //
  // A reviewer demonstrated that with a y-sign flip, which is the case where it bites hardest,
  // because the accuracy dataset's turn is about `+Y` and `diag(1, -1, 1)` commutes with it. The
  // first attempt to fix this added a second sweep about a tilted axis on the theory that
  // non-commutation would expose the flip. It does not: the cancellation above has nothing to do
  // with the axis, so the tilted sweep passed under the mutation too, and the figure this comment
  // briefly claimed for it had never been measured. It is gone rather than weakened.
  //
  // So this test measures the *shift* and is silent about the model. What checks the model is what
  // compares it against something outside itself: the two OpenCV agreement tests above, which the
  // flip fails, and `Unproject.ImageYRunsDownAndCameraYRunsUp`, which is three assertions and no
  // sweep at all.
}

}  // namespace
}  // namespace sphanorama
