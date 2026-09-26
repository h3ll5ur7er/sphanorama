#include "utilities/kept_lens.h"

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

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

// A capture `Refine` fitted: the least is the lens it answers with.
GlobalSolution Fitted(double fx, double spread, double modelError, int32_t width = 640,
                      int32_t height = 480) {
  GlobalSolution capture;
  capture.lensFitted = true;
  capture.focalScale = 1.0;
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

KeptLens Amend(const KeptLens& kept, const GlobalSolution& capture) {
  const Result<KeptLens> amended = AmendKeptLens(kept, capture);
  EXPECT_TRUE(amended.ok()) << amended.status.detail;
  return amended.value;
}

// Whole: the capture's lens, every field of it, and not the lens assumed before it. The capture here
// differs from `Nothing()` in every field a whole copy would carry, and says neither that it was
// estimated nor how sure it is, so that those are the amend's own.
TEST(KeptLens, NothingKeptTakesTheFirstMeasurementWhole) {
  GlobalSolution capture;
  capture.focalScale = 1.25;
  capture.focalSpread = 0.001;
  capture.focalModelError = 0.0005;
  capture.intrinsics = Lens(400.0, 1280, 960);
  capture.intrinsics.fy = 404.0;
  capture.intrinsics.cx = 650.0;
  capture.intrinsics.k1 = 0.07;
  capture.intrinsics.p2 = 0.001;
  capture.intrinsics.estimated = false;
  capture.intrinsics.focalUncertainty = 0.5;
  const KeptLens kept = Amend(Nothing(), capture);
  EXPECT_EQ(kept.captures, 1);
  EXPECT_EQ(kept.lens.fx, 500.0);
  EXPECT_EQ(kept.lens.fy, 505.0);
  EXPECT_EQ(kept.lens.cx, 650.0);
  EXPECT_EQ(kept.lens.cy, 480.0);
  EXPECT_EQ(kept.lens.k1, 0.07);
  EXPECT_EQ(kept.lens.p2, 0.001);
  EXPECT_EQ(kept.lens.width, 1280);
  EXPECT_EQ(kept.lens.height, 960);
  EXPECT_EQ(kept.noise, 0.001);
  EXPECT_EQ(kept.modelError, 0.0005);
  EXPECT_EQ(kept.lens.focalUncertainty, std::hypot(0.001, 0.0005));
  EXPECT_TRUE(kept.lens.estimated);
}

// A capture whose least was not precise, or that reached none, measured nothing it can stand behind.
TEST(KeptLens, ACaptureThatMeasuredNothingLeavesItAlone) {
  const KeptLens before = Amend(Nothing(), Fitted(500.0, 0.001, 0.0005));
  GlobalSolution imprecise = Fitted(510.0, 0.003, 0.0005);
  imprecise.lensFitted = false;
  imprecise.focalScale = 0.0;
  const KeptLens after = Amend(before, imprecise);
  EXPECT_EQ(after.captures, 1);
  EXPECT_EQ(after.lens.fx, 500.0);
  EXPECT_EQ(after.noise, before.noise);
  EXPECT_EQ(after.modelError, before.modelError);
  EXPECT_EQ(after.lens.focalUncertainty, before.lens.focalUncertainty);

  // And the lens it answers with carries the figures kept, whatever the document read back left in
  // the copy: a capture measuring nothing is the commonest amend there is (round 3).
  KeptLens read = before;
  read.lens.focalUncertainty = std::numeric_limits<double>::infinity();
  read.lens.estimated = false;
  const KeptLens settled = Amend(read, imprecise);
  EXPECT_EQ(settled.lens.focalUncertainty, std::hypot(0.001, 0.0005));
  EXPECT_TRUE(settled.lens.estimated);

  // Nor does it start one: the first build on a device can measure nothing.
  KeptLens claimed = Nothing();
  claimed.lens.focalUncertainty = 0.0;
  claimed.lens.estimated = true;
  const KeptLens still = Amend(claimed, imprecise);
  EXPECT_EQ(still.captures, 0);
  EXPECT_EQ(still.lens.fx, 480.0);
  // And its copies say so, whatever it was handed claiming: nothing kept is a guess (round 4).
  EXPECT_TRUE(std::isinf(still.lens.focalUncertainty));
  EXPECT_FALSE(still.lens.estimated);
}

// The kept lens is handed to `Refine`, which answers under it wherever a capture's own fit is not
// surer — as a second capture of the same shape is not, since its model error is the kept lens's.
// Its least is a measurement all the same, and the kept lens is amended with it; otherwise the lens
// would stop learning at the first capture it could not beat (round 1).
TEST(KeptLens, AMeasurementRefineDidNotTakeStillAmendsIt) {
  const KeptLens kept = Amend(Nothing(), Fitted(500.0, 0.001, 0.0004));
  GlobalSolution notTaken;
  notTaken.intrinsics = kept.lens;
  notTaken.lensFitted = false;
  notTaken.focalScale = 506.0 / 500.0;
  notTaken.focalSpread = 0.001;
  notTaken.focalModelError = 0.0004;
  const KeptLens amended = Amend(kept, notTaken);
  EXPECT_EQ(amended.captures, 2);
  EXPECT_NEAR(amended.lens.fx, std::sqrt(500.0 * 506.0), 1e-9);
  EXPECT_NEAR(amended.noise, 0.001 / std::sqrt(2.0), 1e-15);
  EXPECT_NEAR(amended.lens.focalUncertainty, std::hypot(0.001 / std::sqrt(2.0), 0.0004), 1e-15);
}

// The pairs' noise averages down over captures and a lens model's error does not: it is the same
// lens misread the same way every time (ADR 0066).
TEST(KeptLens, AgreeingCapturesShrinkTheNoiseAndNotTheModelError) {
  KeptLens kept = Nothing();
  for (int i = 0; i < 4; ++i) kept = Amend(kept, Fitted(500.0, 0.002, 0.0004));
  EXPECT_EQ(kept.captures, 4);
  EXPECT_NEAR(kept.lens.fx, 500.0, 1e-9);
  EXPECT_NEAR(kept.noise, 0.001, 1e-15);
  EXPECT_NEAR(kept.modelError, 0.0004, 1e-15);
  EXPECT_NEAR(kept.lens.focalUncertainty, std::hypot(0.001, 0.0004), 1e-15);
}

// Where the model errors agree, which they do for a device capturing the plan its lens gives it,
// the surest combination is the inverse-variance one by the noise, in the natural log of the focal
// length, which is what the noise is a standard deviation of.
TEST(KeptLens, WhereTheModelErrorsAgreeEachCaptureCountsByItsNoise) {
  KeptLens kept = Amend(Nothing(), Fitted(500.0, 0.001, 0.0003));
  kept = Amend(kept, Fitted(505.0, 0.002, 0.0003));
  const double wA = 1.0 / (0.001 * 0.001);
  const double wB = 1.0 / (0.002 * 0.002);
  const double logF = (wA * std::log(500.0) + wB * std::log(505.0)) / (wA + wB);
  EXPECT_NEAR(kept.lens.fx, std::exp(logF), 1e-9);
  EXPECT_NEAR(kept.lens.fy, std::exp(logF), 1e-9);
  EXPECT_NEAR(kept.noise, 1.0 / std::sqrt(wA + wB), 1e-15);
  EXPECT_NEAR(kept.modelError, 0.0003, 1e-15);
}

// The model error is a bias, and two captures misreading one lens misread it the same way, so the
// kept figure is the weighted mean of theirs rather than something that averages down. Weighed by
// the noise alone, a quiet lens with a large model error outweighs a slightly noisier capture with
// a small one and leaves the kept lens less sure than the capture was (round 1: 0.142% from a
// capture at 0.048%). Weighed for the least uncertainty, that capture is better alone and is taken
// whole.
TEST(KeptLens, ALensTheCaptureIsBetterWithoutIsReplaced) {
  KeptLens kept = Amend(Nothing(), Fitted(505.0, 0.0001, 0.0019));
  kept = Amend(kept, Fitted(500.0, 0.00014, 0.00046));
  EXPECT_EQ(kept.captures, 2);
  EXPECT_EQ(kept.lens.fx, 500.0);
  EXPECT_EQ(kept.noise, 0.00014);
  EXPECT_EQ(kept.modelError, 0.00046);
  EXPECT_EQ(kept.lens.focalUncertainty, std::hypot(0.00014, 0.00046));
}

// And where each has something the other lacks — less noise, less model error — the weight between
// them is the one that leaves the least uncertainty, which is surer than either and than the weight
// the noise alone gives.
TEST(KeptLens, TheWeightIsTheOneThatLeavesTheLeastUncertainty) {
  const double kNoise = 0.0002, kModel = 0.0004, cNoise = 0.0006, cModel = 0.0002;
  KeptLens kept = Amend(Nothing(), Fitted(505.0, kNoise, kModel));
  kept = Amend(kept, Fitted(500.0, cNoise, cModel));

  // A combination: one weight explains the focal length, the noise and the model error at once.
  const double lambda = (std::log(kept.lens.fx) - std::log(500.0)) / (std::log(505.0) - std::log(500.0));
  ASSERT_GT(lambda, 0.0) << "the premise: the kept lens counted";
  ASSERT_LT(lambda, 1.0) << "the premise: the capture counted";
  EXPECT_NEAR(kept.noise, std::hypot(lambda * kNoise, (1 - lambda) * cNoise), 1e-12);
  EXPECT_NEAR(kept.modelError, lambda * kModel + (1 - lambda) * cModel, 1e-12);
  const auto uncertainty = [&](double l) {
    return std::hypot(std::hypot(l * kNoise, (1 - l) * cNoise), l * kModel + (1 - l) * cModel);
  };
  EXPECT_LT(kept.lens.focalUncertainty, std::hypot(kNoise, kModel));
  const double byNoise = cNoise * cNoise / (kNoise * kNoise + cNoise * cNoise);
  EXPECT_LT(kept.lens.focalUncertainty, uncertainty(byNoise) - 1e-6);
  for (int i = 0; i <= 1000; ++i) {
    EXPECT_GE(uncertainty(i / 1000.0), kept.lens.focalUncertainty - 1e-15) << i;
  }
}

TEST(KeptLens, NeverLessSureThanEitherLensItCombines) {
  const std::vector<double> figures = {0.0, 1e-5, 0.0001, 0.0004, 0.001, 0.002};
  for (const double kNoise : figures) {
    for (const double kModel : figures) {
      for (const double cNoise : figures) {
        for (const double cModel : figures) {
          const KeptLens kept = Amend(Nothing(), Fitted(500.0, kNoise, kModel));
          const KeptLens both = Amend(kept, Fitted(503.0, cNoise, cModel));
          const double surer = std::min(std::hypot(kNoise, kModel), std::hypot(cNoise, cModel));
          // To rounding: a random sweep finds the combination 2e-16 over the surer input, and the
          // focal length an ulp outside the two it lies between (round 2).
          EXPECT_LE(both.lens.focalUncertainty, surer * (1.0 + 1e-15))
              << kNoise << " " << kModel << " " << cNoise << " " << cModel;
          EXPECT_GE(both.lens.fx, 500.0 - 1e-12);
          EXPECT_LE(both.lens.fx, 503.0 + 1e-12);
        }
      }
    }
  }
}

TEST(KeptLens, WhereTheModelErrorsAgreeTheOrderCapturesArriveInDoesNotMatter) {
  const GlobalSolution a = Fitted(500.0, 0.001, 0.0003);
  const GlobalSolution b = Fitted(503.0, 0.0015, 0.0003);
  const GlobalSolution c = Fitted(498.0, 0.004, 0.0003);
  const KeptLens abc = Amend(Amend(Amend(Nothing(), a), b), c);
  const KeptLens cba = Amend(Amend(Amend(Nothing(), c), b), a);
  EXPECT_NEAR(abc.lens.fx, cba.lens.fx, 1e-9);
  EXPECT_NEAR(abc.noise, cba.noise, 1e-15);
  EXPECT_NEAR(abc.modelError, cba.modelError, 1e-15);
}

// The page grabs at whatever size the camera settles on, so a capture can arrive at another size
// than the lens was kept at. The focal length is a length in pixels; its fraction of the long edge
// is what the lens is.
TEST(KeptLens, ACaptureAtAnotherSizeIsReadAtTheKeptOne) {
  const KeptLens kept = Amend(Nothing(), Fitted(500.0, 0.001, 0.0002));
  const KeptLens same = Amend(kept, Fitted(506.0, 0.001, 0.0002));
  const KeptLens doubled = Amend(kept, Fitted(1012.0, 0.001, 0.0002, 1280, 960));
  ASSERT_NEAR(same.lens.fx, std::sqrt(500.0 * 506.0), 1e-9) << "the premise: the capture counted";
  EXPECT_NEAR(doubled.lens.fx, same.lens.fx, 1e-9);
  EXPECT_EQ(doubled.lens.width, 640);
  EXPECT_EQ(doubled.lens.cx, 320.0);

  // Where the capture is taken whole as well as where it is weighed.
  const KeptLens exact = Amend(kept, Fitted(1012.0, 0.0, 0.0, 1280, 960));
  EXPECT_EQ(exact.lens.fx, 506.0);
}

// A frame of another shape is a crop, which the long edge does not describe.
TEST(KeptLens, ACaptureOfAnotherShapeIsRefused) {
  const KeptLens kept = Amend(Nothing(), Fitted(500.0, 0.001, 0.0002));
  const Result<KeptLens> wide = AmendKeptLens(kept, Fitted(500.0, 0.001, 0.0002, 640, 360));
  EXPECT_EQ(wide.status.code, StatusCode::InvalidArgument);
  EXPECT_NE(wide.status.detail.find("shape"), std::string::npos) << wide.status.detail;
  // Turned on its side, too.
  EXPECT_EQ(AmendKeptLens(kept, Fitted(500.0, 0.001, 0.0002, 480, 640)).status.code,
            StatusCode::InvalidArgument);
  // And a shape whose cross products agree only modulo 2^32: 44739884 x 480 and 481 x 640 are both
  // 307840 there, and a stored width is a figure the document can carry wrong.
  EXPECT_EQ(AmendKeptLens(kept, Fitted(500.0, 0.001, 0.0002, 44739884, 481)).status.code,
            StatusCode::InvalidArgument);
}

// Only the focal length is fitted, so it is all a capture amends: the distortion and the principal
// point stay the kept lens's.
TEST(KeptLens, OnlyTheFocalLengthIsAmended) {
  GlobalSolution first = Fitted(500.0, 0.001, 0.0002);
  first.intrinsics.fy = 505.0;
  KeptLens kept = Amend(Nothing(), first);
  GlobalSolution other = Fitted(504.0, 0.001, 0.0002);
  other.intrinsics.k1 = 0.3;
  other.intrinsics.cx = 300.0;
  kept = Amend(kept, other);
  ASSERT_EQ(kept.captures, 2) << "the premise: the capture counted";
  EXPECT_NEAR(kept.lens.fx, std::sqrt(500.0 * 504.0), 1e-9);
  // A lens read back without `estimated` is estimated once amended.
  KeptLens read = Amend(Nothing(), first);
  read.lens.estimated = false;
  EXPECT_TRUE(Amend(read, other).lens.estimated);
  // Pixels that are not square stay as they were: `fy` moves in proportion with `fx`.
  EXPECT_NEAR(kept.lens.fy / kept.lens.fx, 1.01, 1e-12);
  EXPECT_EQ(kept.lens.k1, -0.02);
  EXPECT_EQ(kept.lens.cx, 320.0);
}

// A lens known exactly stays exactly as it is, and a capture known exactly replaces a lens that is
// not. Exact is the two figures together, not the noise alone: a lens with no noise and a model
// error is not exact, and a capture with less of both replaces it (round 1).
TEST(KeptLens, ExactIsBothFiguresZero) {
  KeptLens exact = Amend(Nothing(), Fitted(500.0, 0.0, 0.0));
  exact = Amend(exact, Fitted(510.0, 0.001, 0.0002));
  EXPECT_EQ(exact.captures, 2);
  EXPECT_EQ(exact.lens.fx, 500.0);
  EXPECT_EQ(exact.noise, 0.0);
  EXPECT_EQ(exact.modelError, 0.0);

  KeptLens loose = Amend(Nothing(), Fitted(510.0, 0.001, 0.0002));
  loose = Amend(loose, Fitted(500.0, 0.0, 0.0));
  EXPECT_EQ(loose.lens.fx, 500.0);
  EXPECT_EQ(loose.lens.focalUncertainty, 0.0);

  KeptLens quiet = Amend(Nothing(), Fitted(500.0, 0.0, 0.0005));
  quiet = Amend(quiet, Fitted(510.0, 0.0001, 0.0001));
  EXPECT_NEAR(quiet.lens.fx, 510.0, 1e-9);
  EXPECT_NEAR(quiet.lens.focalUncertainty, std::hypot(0.0001, 0.0001), 1e-15);

  // Two with no noise and one model error are equally sure, and no weight between them is better
  // than another; the kept lens is not moved, rather than divided by nothing.
  KeptLens even = Amend(Nothing(), Fitted(500.0, 0.0, 0.0005));
  even = Amend(even, Fitted(510.0, 0.0, 0.0005));
  EXPECT_EQ(even.lens.fx, 500.0);
  EXPECT_EQ(even.lens.focalUncertainty, 0.0005);

  // And two exact lenses that disagree: nothing to scale by, and the kept one stands.
  KeptLens both = Amend(Nothing(), Fitted(500.0, 0.0, 0.0));
  both = Amend(both, Fitted(510.0, 0.0, 0.0));
  EXPECT_EQ(both.lens.fx, 500.0);
}

// The weight is a ratio of squares, and a square of a figure small or large enough leaves the
// doubles: 1e-170 squared is zero and 1e170 squared is infinite. Equal figures weigh equally at any
// scale.
TEST(KeptLens, TheWeightDoesNotDependOnTheScaleOfTheFigures) {
  for (const double figure : {1e-170, 1e-5, 1e170}) {
    const KeptLens kept = Amend(Amend(Nothing(), Fitted(500.0, figure, figure)),
                                Fitted(505.0, figure, figure));
    EXPECT_NEAR(kept.lens.fx, std::sqrt(500.0 * 505.0), 1e-9) << figure;
    EXPECT_NEAR(kept.noise / figure, std::sqrt(0.5), 1e-12) << figure;
    EXPECT_NEAR(kept.modelError / figure, 1.0, 1e-12) << figure;
  }
  // Whichever of the four is the large one sets the scale: scaled by any other, it squares past
  // the doubles (round 2).
  for (int large = 0; large < 4; ++large) {
    double f[4] = {1e-170, 1e-170, 1e-170, 1e-170};
    f[large] = 1e-3;
    const KeptLens kept =
        Amend(Amend(Nothing(), Fitted(500.0, f[0], f[1])), Fitted(505.0, f[2], f[3]));
    EXPECT_TRUE(std::isfinite(kept.lens.fx)) << large;
    EXPECT_LE(kept.lens.focalUncertainty, 1e-3) << large;
  }
}

TEST(KeptLens, TheCountStopsRatherThanOverflowing) {
  KeptLens kept = Amend(Nothing(), Fitted(500.0, 0.001, 0.0002));
  kept.captures = std::numeric_limits<int32_t>::max();
  kept = Amend(kept, Fitted(505.0, 0.001, 0.0002));
  EXPECT_EQ(kept.captures, std::numeric_limits<int32_t>::max());
  EXPECT_NEAR(kept.lens.fx, std::sqrt(500.0 * 505.0), 1e-9) << "the premise: the capture counted";
}

// A kept lens is read back from storage, and a capture from a solve; either can arrive malformed,
// and an answer made from one would be a NaN focal length every later `Refine` refuses.
TEST(KeptLens, MalformedInputIsRefused) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const KeptLens kept = Amend(Nothing(), Fitted(500.0, 0.001, 0.0002));
  const GlobalSolution capture = Fitted(505.0, 0.001, 0.0002);
  ASSERT_TRUE(AmendKeptLens(kept, capture).ok()) << "the premise: valid in every other respect";

  struct Row {
    std::string why;
    bool spoilsKept;
    std::function<void(KeptLens&, GlobalSolution&)> spoil;
  };
  const std::vector<Row> rows = {
      {"a negative count", true, [](KeptLens& k, GlobalSolution&) { k.captures = -1; }},
      {"a kept lens that cannot project", true, [](KeptLens& k, GlobalSolution&) { k.lens.fx = 0.0; }},
      {"kept noise NaN", true, [&](KeptLens& k, GlobalSolution&) { k.noise = nan; }},
      {"kept noise below zero", true, [](KeptLens& k, GlobalSolution&) { k.noise = -0.001; }},
      {"kept noise infinite", true, [&](KeptLens& k, GlobalSolution&) { k.noise = inf; }},
      {"kept model error NaN", true, [&](KeptLens& k, GlobalSolution&) { k.modelError = nan; }},
      {"kept model error below zero", true, [](KeptLens& k, GlobalSolution&) { k.modelError = -0.001; }},
      {"kept model error infinite", true, [&](KeptLens& k, GlobalSolution&) { k.modelError = inf; }},
      {"a scale NaN", false, [&](KeptLens&, GlobalSolution& c) { c.focalScale = nan; }},
      {"a scale below zero", false, [](KeptLens&, GlobalSolution& c) { c.focalScale = -1.0; }},
      {"a scale infinite", false, [&](KeptLens&, GlobalSolution& c) { c.focalScale = inf; }},
      {"a capture that cannot project", false, [](KeptLens&, GlobalSolution& c) { c.intrinsics.fy = -1.0; }},
      {"a spread NaN", false, [&](KeptLens&, GlobalSolution& c) { c.focalSpread = nan; }},
      {"a spread below zero", false, [](KeptLens&, GlobalSolution& c) { c.focalSpread = -0.001; }},
      {"a spread infinite", false, [&](KeptLens&, GlobalSolution& c) { c.focalSpread = inf; }},
      {"a model error NaN", false, [&](KeptLens&, GlobalSolution& c) { c.focalModelError = nan; }},
      {"a model error below zero", false, [](KeptLens&, GlobalSolution& c) { c.focalModelError = -0.001; }},
      {"a model error infinite", false, [&](KeptLens&, GlobalSolution& c) { c.focalModelError = inf; }},
  };
  for (const Row& row : rows) {
    KeptLens k = kept;
    GlobalSolution c = capture;
    row.spoil(k, c);
    const Result<KeptLens> amended = AmendKeptLens(k, c);
    EXPECT_EQ(amended.status.code, StatusCode::InvalidArgument) << row.why;
  }

  // Nothing kept means nothing is read from it; a capture that measured nothing is not read either.
  KeptLens nothing = Nothing();
  nothing.lens.fx = nan;
  nothing.noise = nan;
  EXPECT_TRUE(AmendKeptLens(nothing, capture).ok());
  GlobalSolution measuredNothing = capture;
  measuredNothing.focalScale = 0.0;
  measuredNothing.focalSpread = inf;
  measuredNothing.focalModelError = nan;
  EXPECT_TRUE(AmendKeptLens(kept, measuredNothing).ok());
  // But a kept lens is read whatever the capture measured: a corrupt one is refused, not handed back.
  for (const Row& row : rows) {
    if (!row.spoilsKept) continue;
    KeptLens k = kept;
    GlobalSolution unused = capture;
    row.spoil(k, unused);
    EXPECT_EQ(AmendKeptLens(k, measuredNothing).status.code, StatusCode::InvalidArgument) << row.why;
  }
}

// What `Refine` is handed. `lens.focalUncertainty` is `hypot(noise, modelError)` over again, and a
// kept lens read back from a document need not carry it right: left at its default it reads as a
// guess, which any precise fit replaces, and zeroed it reads as exact, which none does (round 2).
// So the figure handed in is derived from the two that are kept, never read from the copy.
TEST(KeptLens, TheLensHandedToRefineDerivesItsUncertainty) {
  const KeptLens kept = Amend(Nothing(), Fitted(500.0, 0.001, 0.0002));
  for (const double copy : {std::numeric_limits<double>::infinity(), 0.0, 5.0}) {
    KeptLens read = kept;
    read.lens.focalUncertainty = copy;
    read.lens.estimated = false;
    const Result<Intrinsics> lens = KeptLensFor(read, 640, 480);
    ASSERT_TRUE(lens.ok()) << lens.status.detail;
    EXPECT_EQ(lens.value.focalUncertainty, std::hypot(0.001, 0.0002)) << copy;
    EXPECT_EQ(lens.value.fx, 500.0);
    EXPECT_TRUE(lens.value.estimated);
  }
}

// A capture's matches are pixels of the frame it was grabbed at, and the lens it is handed must be
// that frame's: the kept one at another size of its shape, every length in pixels scaled by the
// ratio of the long edges and the distortion, which is in normalised coordinates, as it was.
TEST(KeptLens, TheLensHandedToRefineIsAtTheCapturesSize) {
  GlobalSolution first = Fitted(500.0, 0.001, 0.0002);
  first.intrinsics.fy = 505.0;
  first.intrinsics.cx = 322.0;
  first.intrinsics.cy = 236.0;
  first.intrinsics.k2 = 0.004;
  first.intrinsics.k3 = -0.0007;
  first.intrinsics.p1 = 0.0003;
  first.intrinsics.p2 = -0.0002;
  first.intrinsics.rollingShutterLineTimeNs = 15000.0;
  const KeptLens kept = Amend(Nothing(), first);
  const Result<Intrinsics> doubled = KeptLensFor(kept, 1280, 960);
  ASSERT_TRUE(doubled.ok()) << doubled.status.detail;
  EXPECT_EQ(doubled.value.width, 1280);
  EXPECT_EQ(doubled.value.height, 960);
  EXPECT_EQ(doubled.value.fx, 1000.0);
  EXPECT_EQ(doubled.value.fy, 1010.0);
  EXPECT_EQ(doubled.value.cx, 644.0);
  EXPECT_EQ(doubled.value.cy, 472.0);
  EXPECT_EQ(doubled.value.k1, -0.02);
  EXPECT_EQ(doubled.value.k2, 0.004);
  EXPECT_EQ(doubled.value.k3, -0.0007);
  EXPECT_EQ(doubled.value.p1, 0.0003);
  EXPECT_EQ(doubled.value.p2, -0.0002);
  // Not a length: whether a mode at another size reads its rows faster is the sensor's to say, and
  // nothing reads it yet.
  EXPECT_EQ(doubled.value.rollingShutterLineTimeNs, 15000.0);
  EXPECT_EQ(doubled.value.focalUncertainty, std::hypot(0.001, 0.0002));

  // And a capture measured at that size amends the kept lens back at its own.
  GlobalSolution atDouble;
  atDouble.intrinsics = doubled.value;
  atDouble.focalScale = 1.0;
  atDouble.focalSpread = 0.0;
  atDouble.focalModelError = 0.0;
  EXPECT_EQ(Amend(kept, atDouble).lens.fx, 500.0);
}

TEST(KeptLens, NoLensIsHandedToRefineWhereNoneIsKept) {
  EXPECT_EQ(KeptLensFor(Nothing(), 640, 480).status.code, StatusCode::NotFound);
  // An unset size is refused whether or not a lens is kept: `NotFound` tells the caller to go on
  // from its guess, and a frame of no size is nothing to go on with (round 3).
  EXPECT_EQ(KeptLensFor(Nothing(), 0, 0).status.code, StatusCode::InvalidArgument);
  EXPECT_EQ(KeptLensFor(Nothing(), -5, 7).status.code, StatusCode::InvalidArgument);
  const KeptLens kept = Amend(Nothing(), Fitted(500.0, 0.001, 0.0002));
  EXPECT_EQ(KeptLensFor(kept, 640, 360).status.code, StatusCode::InvalidArgument);
  // Turned on its side is another shape too: the principal point would be off-centre by the
  // difference of the edges (round 3).
  EXPECT_EQ(KeptLensFor(kept, 480, 640).status.code, StatusCode::InvalidArgument);
  EXPECT_EQ(KeptLensFor(kept, 0, 0).status.code, StatusCode::InvalidArgument);
  // A kept lens that is not one is the stored document's fault, not the call's, and has a code of
  // its own: the caller discards the document on it and on nothing else (round 4).
  KeptLens spoiled = kept;
  spoiled.noise = -1.0;
  EXPECT_EQ(KeptLensFor(spoiled, 640, 480).status.code, StatusCode::FailedPrecondition);
  spoiled = kept;
  spoiled.lens.fx = 0.0;
  EXPECT_EQ(KeptLensFor(spoiled, 640, 480).status.code, StatusCode::FailedPrecondition);
  spoiled = kept;
  spoiled.captures = -1;
  EXPECT_EQ(KeptLensFor(spoiled, 640, 480).status.code, StatusCode::FailedPrecondition);
}

// Two finite figures can combine past the largest double, and an infinite uncertainty is a guess —
// the one reading a kept lens must never have, since any fit replaces it (round 2).
TEST(KeptLens, FiguresThatCombinePastTheDoublesAreRefused) {
  const double huge = 1.5e308;
  EXPECT_EQ(AmendKeptLens(Nothing(), Fitted(500.0, huge, huge)).status.code,
            StatusCode::InvalidArgument);
  // Combined as independent errors, not added: 1e308 and 1e308 make 1.41e308, which is a figure.
  EXPECT_TRUE(AmendKeptLens(Nothing(), Fitted(500.0, 1e308, 1e308)).ok());
  KeptLens kept = Amend(Nothing(), Fitted(500.0, 0.001, 0.0002));
  kept.noise = huge;
  kept.modelError = huge;
  EXPECT_EQ(AmendKeptLens(kept, Fitted(505.0, 0.001, 0.0002)).status.code,
            StatusCode::InvalidArgument);
  EXPECT_EQ(KeptLensFor(kept, 640, 480).status.code, StatusCode::FailedPrecondition);
}

// A lens computed from valid inputs can still fail to be one: a scale or a size ratio can carry a
// focal length past the doubles, and a principal point just inside a small frame can
// round onto the edge of a larger one. Neither is kept nor handed on (round 3).
TEST(KeptLens, ALensThatCannotProjectIsNeitherKeptNorHandedOn) {
  GlobalSolution huge = Fitted(1000.0, 0.001, 0.0002);
  huge.focalScale = 1e306;
  EXPECT_EQ(AmendKeptLens(Nothing(), huge).status.code, StatusCode::InvalidArgument);
  const KeptLens kept = Amend(Nothing(), Fitted(500.0, 0.001, 0.0002));
  EXPECT_EQ(AmendKeptLens(kept, huge).status.code, StatusCode::InvalidArgument);

  KeptLens far = kept;
  far.lens.fx = 1e308;
  far.lens.fy = 1e308;
  ASSERT_TRUE(KeptLensFor(far, 640, 480).ok()) << "the premise: usable at its own size";
  EXPECT_EQ(KeptLensFor(far, 1280, 960).status.code, StatusCode::InvalidArgument);

  KeptLens small = Amend(Nothing(), Fitted(5.0, 0.001, 0.0002, 3, 3));
  small.lens.cx = std::nextafter(3.0, 0.0);
  ASSERT_TRUE(KeptLensFor(small, 3, 3).ok()) << "the premise: usable at its own size";
  EXPECT_EQ(KeptLensFor(small, 17, 17).status.code, StatusCode::InvalidArgument);
}

}  // namespace
}  // namespace sphanorama
