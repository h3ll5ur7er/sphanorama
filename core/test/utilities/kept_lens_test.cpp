#include "utilities/kept_lens.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>

namespace sphanorama {
namespace {

Intrinsics Lens(double fx, int32_t width = 640, int32_t height = 480) {
  Intrinsics lens;
  lens.fx = fx;
  lens.fy = fx;
  lens.cx = width / 2.0;
  lens.cy = height / 2.0;
  lens.width = width;
  lens.height = height;
  lens.k1 = -0.02;
  return lens;
}

GlobalSolution Fitted(double fx, double spread, double modelError, int32_t width = 640,
                      int32_t height = 480) {
  GlobalSolution capture;
  capture.lensFitted = true;
  capture.intrinsics = Lens(fx, width, height);
  capture.intrinsics.estimated = true;
  capture.intrinsics.focalUncertainty = std::hypot(spread, modelError);
  capture.focalSpread = spread;
  capture.focalModelError = modelError;
  return capture;
}

KeptLens Nothing() {
  KeptLens kept;
  kept.lens = Lens(480.0);
  return kept;
}

TEST(KeptLens, NothingKeptTakesTheFirstFitWhole) {
  const KeptLens kept = AmendKeptLens(Nothing(), Fitted(500.0, 0.001, 0.0005));
  EXPECT_EQ(kept.captures, 1);
  EXPECT_EQ(kept.lens.fx, 500.0);
  EXPECT_EQ(kept.lens.fy, 500.0);
  EXPECT_EQ(kept.noise, 0.001);
  EXPECT_EQ(kept.modelError, 0.0005);
  EXPECT_EQ(kept.lens.focalUncertainty, std::hypot(0.001, 0.0005));
  EXPECT_TRUE(kept.lens.estimated);
}

// A capture `Refine` did not fit says nothing about the focal length it can stand behind: its least
// was not precise enough, or not surer than the lens it was handed, and its rotations were not
// solved under it.
TEST(KeptLens, ACaptureNotFittedLeavesItAlone) {
  const KeptLens before = AmendKeptLens(Nothing(), Fitted(500.0, 0.001, 0.0005));
  GlobalSolution refused = Fitted(510.0, 0.003, 0.0005);
  refused.lensFitted = false;
  const KeptLens after = AmendKeptLens(before, refused);
  EXPECT_EQ(after.captures, 1);
  EXPECT_EQ(after.lens.fx, 500.0);
  EXPECT_EQ(after.noise, before.noise);
}

// The pairs' noise averages down over captures and a lens model's error does not: it is the same
// lens misread the same way every time (ADR 0066).
TEST(KeptLens, AgreeingCapturesShrinkTheNoiseAndNotTheModelError) {
  KeptLens kept = Nothing();
  for (int i = 0; i < 4; ++i) kept = AmendKeptLens(kept, Fitted(500.0, 0.002, 0.0004));
  EXPECT_EQ(kept.captures, 4);
  EXPECT_NEAR(kept.lens.fx, 500.0, 1e-9);
  EXPECT_NEAR(kept.noise, 0.001, 1e-15);
  EXPECT_NEAR(kept.modelError, 0.0004, 1e-15);
  EXPECT_NEAR(kept.lens.focalUncertainty, std::hypot(0.001, 0.0004), 1e-15);
}

// Weighed by each capture's own noise, in the natural log of the focal length, which is what the
// noise is a standard deviation of.
TEST(KeptLens, EachCaptureCountsByItsOwnNoise) {
  KeptLens kept = AmendKeptLens(Nothing(), Fitted(500.0, 0.001, 0.0002));
  kept = AmendKeptLens(kept, Fitted(505.0, 0.002, 0.0006));
  const double wA = 1.0 / (0.001 * 0.001);
  const double wB = 1.0 / (0.002 * 0.002);
  const double logF = (wA * std::log(500.0) + wB * std::log(505.0)) / (wA + wB);
  EXPECT_NEAR(kept.lens.fx, std::exp(logF), 1e-9);
  EXPECT_NEAR(kept.lens.fy, std::exp(logF), 1e-9);
  EXPECT_NEAR(kept.noise, 1.0 / std::sqrt(wA + wB), 1e-15);
  // The bias of that weighted mean is the same weighting of each capture's own.
  EXPECT_NEAR(kept.modelError, (wA * 0.0002 + wB * 0.0006) / (wA + wB), 1e-15);
}

TEST(KeptLens, TheOrderCapturesArriveInDoesNotMatter) {
  const GlobalSolution a = Fitted(500.0, 0.001, 0.0002);
  const GlobalSolution b = Fitted(503.0, 0.0015, 0.0003);
  const GlobalSolution c = Fitted(498.0, 0.004, 0.0009);
  const KeptLens abc = AmendKeptLens(AmendKeptLens(AmendKeptLens(Nothing(), a), b), c);
  const KeptLens cba = AmendKeptLens(AmendKeptLens(AmendKeptLens(Nothing(), c), b), a);
  EXPECT_NEAR(abc.lens.fx, cba.lens.fx, 1e-9);
  EXPECT_NEAR(abc.noise, cba.noise, 1e-15);
  EXPECT_NEAR(abc.modelError, cba.modelError, 1e-15);
}

// The page grabs at whatever size the camera settles on, so a capture can arrive at another size
// than the lens was kept at. The focal length is a length in pixels; its fraction of the long edge
// is what the lens is.
TEST(KeptLens, ACaptureAtAnotherSizeIsReadAtTheKeptOne) {
  const KeptLens kept = AmendKeptLens(Nothing(), Fitted(500.0, 0.001, 0.0002));
  const KeptLens same = AmendKeptLens(kept, Fitted(506.0, 0.001, 0.0002));
  const KeptLens doubled = AmendKeptLens(kept, Fitted(1012.0, 0.001, 0.0002, 1280, 960));
  ASSERT_NEAR(same.lens.fx, std::sqrt(500.0 * 506.0), 1e-9) << "the premise: the capture counted";
  EXPECT_NEAR(doubled.lens.fx, same.lens.fx, 1e-9);
  EXPECT_EQ(doubled.lens.width, 640);
  EXPECT_EQ(doubled.lens.cx, 320.0);
}

// Only the focal length is fitted, so it is all a capture amends: the distortion and the principal
// point stay the kept lens's.
TEST(KeptLens, OnlyTheFocalLengthIsAmended) {
  KeptLens kept = AmendKeptLens(Nothing(), Fitted(500.0, 0.001, 0.0002));
  GlobalSolution other = Fitted(504.0, 0.001, 0.0002);
  other.intrinsics.k1 = 0.3;
  other.intrinsics.cx = 300.0;
  kept = AmendKeptLens(kept, other);
  ASSERT_EQ(kept.captures, 2) << "the premise: the capture counted";
  EXPECT_NEAR(kept.lens.fx, std::sqrt(500.0 * 504.0), 1e-9);
  EXPECT_EQ(kept.lens.k1, -0.02);
  EXPECT_EQ(kept.lens.cx, 320.0);
}

// A lens known exactly stays exactly as it is, and a capture known exactly replaces a lens that is
// not: an infinite weight is not something to average.
TEST(KeptLens, ExactIsExact) {
  KeptLens exact = AmendKeptLens(Nothing(), Fitted(500.0, 0.0, 0.0));
  exact = AmendKeptLens(exact, Fitted(510.0, 0.001, 0.0002));
  EXPECT_EQ(exact.lens.fx, 500.0);
  EXPECT_EQ(exact.noise, 0.0);

  KeptLens loose = AmendKeptLens(Nothing(), Fitted(510.0, 0.001, 0.0002));
  loose = AmendKeptLens(loose, Fitted(500.0, 0.0, 0.0001));
  EXPECT_EQ(loose.lens.fx, 500.0);
  EXPECT_EQ(loose.noise, 0.0);
  EXPECT_EQ(loose.modelError, 0.0001);
}

}  // namespace
}  // namespace sphanorama
