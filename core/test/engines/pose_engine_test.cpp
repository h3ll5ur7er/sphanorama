// The pose engine (V5), as far as it goes today: absolute orientation passed through, gyro rates
// integrated, stability from how fast the device is moving. Visual correction is Phase 2.
//
// A fused estimate's output is not knowable in advance, but these are: integrating nothing
// changes nothing, integrating a known rate for a known time turns by a known angle, and an
// engine that cannot estimate says so rather than guessing.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "engines/pose_engine/orientation_pose_engine.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr double kRadToDeg = 57.29577951308232;

ImuSample Oriented(int64_t timestampNs, double azimuthDeg, double elevationDeg) {
  ImuSample sample;
  sample.timestampNs = timestampNs;
  sample.hasOrientation = true;
  sample.orientation = FromAzimuthElevation(azimuthDeg, elevationDeg);
  return sample;
}

ImuSample Spinning(int64_t timestampNs, double yawRateRadPerSec) {
  ImuSample sample;
  sample.timestampNs = timestampNs;
  sample.angularVelocity = Vec3{0, yawRateRadPerSec, 0};
  return sample;
}

// A sample from a device that reports both: a fused attitude and the gyroscope underneath it.
// Nothing produces one today — the browser adapter reports OrientationOnly and no rates — which
// is exactly why the engine's behaviour on them has to be decided before one arrives.
ImuSample Fused(int64_t timestampNs, double azimuthDeg, double elevationDeg, const Vec3& rate) {
  ImuSample sample = Oriented(timestampNs, azimuthDeg, elevationDeg);
  sample.angularVelocity = rate;
  return sample;
}

PoseState Started(OrientationPoseEngine& engine,
                  MotionCapability capability = MotionCapability::OrientationOnly) {
  auto initial = engine.Initial(PoseMode::Fused, capability);
  EXPECT_TRUE(initial.ok());
  return initial.value;
}

TEST(PoseEngine, StartsAtIdentityWithNoConfidence) {
  // Nothing has been observed yet, and a confidence of zero is how a caller learns that rather
  // than by trusting an orientation nothing produced.
  OrientationPoseEngine engine;
  PoseState state = Started(engine);
  auto pose = engine.Integrate(state, {});
  ASSERT_TRUE(pose.ok());
  EXPECT_NEAR(AngleBetween(pose.value.pose.orientation, Quat{}), 0.0, 1e-12);
  EXPECT_DOUBLE_EQ(pose.value.pose.confidence, 0.0);
}

TEST(PoseEngine, PassesAnAbsoluteOrientationStraightThrough) {
  // The browser reports a fused orientation, not a rate. Re-deriving one from it would add drift
  // to a value the platform already got right.
  OrientationPoseEngine engine;
  PoseState state = Started(engine);
  const std::vector<ImuSample> samples{Oriented(1'000'000, 42.0, -10.0)};
  auto pose = engine.Integrate(state, samples);
  ASSERT_TRUE(pose.ok());
  EXPECT_NEAR(AngleBetween(pose.value.pose.orientation, FromAzimuthElevation(42.0, -10.0)), 0.0, 1e-9);
  EXPECT_GT(pose.value.pose.confidence, 0.9);
}

TEST(PoseEngine, TakesTheLatestOrientationFromABatch) {
  OrientationPoseEngine engine;
  PoseState state = Started(engine);
  const std::vector<ImuSample> samples{
      Oriented(1'000'000, 10.0, 0.0),
      Oriented(2'000'000, 20.0, 0.0),
      Oriented(3'000'000, 30.0, 0.0),
  };
  auto pose = engine.Integrate(state, samples);
  ASSERT_TRUE(pose.ok());
  EXPECT_NEAR(AngleBetween(pose.value.pose.orientation, FromAzimuthElevation(30.0, 0.0)), 0.0, 1e-9);
  EXPECT_EQ(pose.value.pose.timestampNs, 3'000'000);
}

TEST(PoseEngine, RemembersTheLastOrientationAcrossEmptyBatches) {
  // The capture loop calls this every frame whether or not the sensor produced anything; losing
  // the pose on an empty batch would make the reticle flick back to centre between samples.
  OrientationPoseEngine engine;
  PoseState state = Started(engine);
  const std::vector<ImuSample> samples{Oriented(1'000'000, 42.0, 0.0)};
  auto seen = engine.Integrate(state, samples);
  ASSERT_TRUE(seen.ok());

  auto pose = engine.Integrate(seen.value, {});
  ASSERT_TRUE(pose.ok());
  EXPECT_NEAR(AngleBetween(pose.value.pose.orientation, FromAzimuthElevation(42.0, 0.0)), 0.0, 1e-9);
}

TEST(PoseEngine, IntegratesAKnownRateOverAKnownTime) {
  // A quarter turn per second for one second is a quarter turn. Drift aside, this is the one
  // thing gyro integration must get right.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  const double rate = 1.5707963267948966;   // pi/2 rad/s
  const std::vector<ImuSample> samples{Spinning(0, rate), Spinning(1'000'000'000, rate)};

  auto pose = engine.Integrate(state, samples);
  ASSERT_TRUE(pose.ok());
  EXPECT_NEAR(AngleBetween(pose.value.pose.orientation, Quat{}) * kRadToDeg, 90.0, 0.5);
}

TEST(PoseEngine, IntegratingZeroRateChangesNothing) {
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  const std::vector<ImuSample> samples{Spinning(0, 0.0), Spinning(500'000'000, 0.0)};
  auto pose = engine.Integrate(state, samples);
  ASSERT_TRUE(pose.ok());
  EXPECT_NEAR(AngleBetween(pose.value.pose.orientation, Quat{}), 0.0, 1e-9);
}

TEST(PoseEngine, IntegrationAccumulatesAcrossCalls) {
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  const double rate = 0.7853981633974483;   // pi/4 rad/s
  for (int i = 0; i < 2; ++i) {
    const std::vector<ImuSample> samples{
        Spinning(static_cast<int64_t>(i) * 1'000'000'000, rate),
        Spinning(static_cast<int64_t>(i + 1) * 1'000'000'000, rate)};
    auto advanced = engine.Integrate(state, samples);
    ASSERT_TRUE(advanced.ok());
    state = advanced.value;
  }
  auto pose = engine.Integrate(state, {});
  EXPECT_NEAR(AngleBetween(pose.value.pose.orientation, Quat{}) * kRadToDeg, 90.0, 1.0);
}

TEST(PoseEngine, AnAbsoluteReadingOverridesAccumulatedDrift) {
  // Integration drifts; an absolute reading is ground truth and must win, or the two sources
  // would fight and the reticle would lag behind the phone.
  OrientationPoseEngine engine;
  PoseState state = Started(engine);
  const std::vector<ImuSample> spun{Spinning(0, 1.0), Spinning(1'000'000'000, 1.0)};
  auto drifted = engine.Integrate(state, spun);
  ASSERT_TRUE(drifted.ok());

  const std::vector<ImuSample> corrected{Oriented(2'000'000'000, 0.0, 0.0)};
  auto pose = engine.Integrate(drifted.value, corrected);
  ASSERT_TRUE(pose.ok());
  EXPECT_NEAR(AngleBetween(pose.value.pose.orientation, Quat{}), 0.0, 1e-9);
}

TEST(PoseEngine, IntegratingIsPureInThePriorState) {
  // The property the whole stateless contract exists for: the same prior and the same samples
  // give the same answer, every time and in any order. It is what lets a recorded session be
  // replayed through a candidate fusion filter to compare it against this one.
  OrientationPoseEngine engine;
  const PoseState state = Started(engine, MotionCapability::GyroAccel);
  const std::vector<ImuSample> samples{Spinning(0, 1.0), Spinning(1'000'000'000, 1.0)};

  auto first = engine.Integrate(state, samples);
  auto second = engine.Integrate(state, samples);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_NEAR(AngleBetween(first.value.pose.orientation, second.value.pose.orientation), 0.0,
              1e-15);
  EXPECT_DOUBLE_EQ(first.value.pose.confidence, second.value.pose.confidence);
  // And the prior it was handed is untouched, so the caller still owns its own state.
  EXPECT_FALSE(state.observed);
}

TEST(PoseEngine, AFreshStateHasSeenNothing) {
  OrientationPoseEngine engine;
  auto initial = engine.Initial(PoseMode::GyroOnly, MotionCapability::GyroAccel);
  ASSERT_TRUE(initial.ok());
  EXPECT_FALSE(initial.value.observed);
  EXPECT_FALSE(initial.value.absolute);
  EXPECT_DOUBLE_EQ(initial.value.pose.confidence, 0.0);
  // The mode and capability come back in the state rather than being remembered by the engine.
  EXPECT_EQ(initial.value.mode, PoseMode::GyroOnly);
  EXPECT_EQ(initial.value.capability, MotionCapability::GyroAccel);
}

TEST(PoseEngine, DeadReckoningAfterAnAbsoluteFixDropsBackToHalfConfidence) {
  // Confidence is the only thing telling a caller whether the pose was measured or guessed. Once
  // an absolute reading has arrived, integrating rates on top of it is guessing again — reporting
  // 1.0 there would make an hour of accumulating drift look like ground truth.
  OrientationPoseEngine engine;
  const PoseState state = Started(engine, MotionCapability::GyroAccelMag);
  const std::vector<ImuSample> fixed{Oriented(1'000'000'000, 0.0, 0.0)};
  auto measured = engine.Integrate(state, fixed);
  ASSERT_TRUE(measured.ok());
  EXPECT_DOUBLE_EQ(measured.value.pose.confidence, 1.0);

  const std::vector<ImuSample> spun{Spinning(2'000'000'000, 1.0)};
  auto guessed = engine.Integrate(measured.value, spun);
  ASSERT_TRUE(guessed.ok());
  EXPECT_DOUBLE_EQ(guessed.value.pose.confidence, 0.5);
  EXPECT_FALSE(guessed.value.absolute);
}

TEST(PoseEngine, AStillDeviceIsPerfectlyStable) {
  OrientationPoseEngine engine;
  const std::vector<ImuSample> samples{Spinning(0, 0.0), Spinning(100'000'000, 0.0)};
  auto stability = engine.Stability(samples);
  ASSERT_TRUE(stability.ok());
  EXPECT_NEAR(stability.value, 1.0, 1e-9);
}

TEST(PoseEngine, AMovingDeviceIsNot) {
  // Firing a burst mid-swing is how a cell ends up with five blurred frames and no good one.
  OrientationPoseEngine engine;
  const std::vector<ImuSample> samples{Spinning(0, 3.0), Spinning(100'000'000, 3.0)};
  auto stability = engine.Stability(samples);
  ASSERT_TRUE(stability.ok());
  EXPECT_LT(stability.value, 0.2);
}

TEST(PoseEngine, StabilityStaysWithinItsRange) {
  OrientationPoseEngine engine;
  for (double rate : {0.0, 0.05, 0.5, 5.0, 50.0}) {
    const std::vector<ImuSample> samples{Spinning(0, rate), Spinning(50'000'000, rate)};
    auto stability = engine.Stability(samples);
    ASSERT_TRUE(stability.ok());
    EXPECT_GE(stability.value, 0.0);
    EXPECT_LE(stability.value, 1.0);
  }
}

TEST(PoseEngine, StabilityOfNothingIsUnknownRatherThanPerfect) {
  // Reporting a still device when no samples arrived would let a burst fire during a dropout.
  OrientationPoseEngine engine;
  EXPECT_FALSE(engine.Stability({}).ok());
}

TEST(PoseEngine, VisualCorrectionIsNotAttemptedYet) {
  OrientationPoseEngine engine;
  PoseSample prior;
  prior.orientation = FromAzimuthElevation(15.0, 0.0);
  auto corrected = engine.Correct(FrameRef{}, FrameRef{}, prior);
  ASSERT_TRUE(corrected.ok());
  EXPECT_FALSE(corrected.value.visuallyCorrected);
  EXPECT_NEAR(AngleBetween(corrected.value.orientation, prior.orientation), 0.0, 1e-12);
}


TEST(PoseEngine, ASwingingPhoneIsUnstableEvenWithNoRatesToReadIt) {
  // The browser reports an attitude and no rates at all, so every sample's angularVelocity is a
  // zero standing in for "not measured". Reading those zeros as a measurement made Stability
  // report 1.0 for a phone being whipped around — and stability is what gates firing a burst, so
  // the one reading that must never be wrong was wrong in exactly the direction that hurts.
  OrientationPoseEngine engine;
  const std::vector<ImuSample> swung{
      Oriented(0, 0.0, 0.0),
      Oriented(100'000'000, 30.0, 0.0),    // 30 degrees in a tenth of a second
      Oriented(200'000'000, 60.0, 0.0),
  };
  auto stability = engine.Stability(swung);
  ASSERT_TRUE(stability.ok());
  EXPECT_LT(stability.value, 0.2);
}

TEST(PoseEngine, APhoneHeldStillIsStableFromOrientationsAlone) {
  OrientationPoseEngine engine;
  const std::vector<ImuSample> held{
      Oriented(0, 42.0, 10.0),
      Oriented(100'000'000, 42.0, 10.0),
      Oriented(200'000'000, 42.0, 10.0),
  };
  auto stability = engine.Stability(held);
  ASSERT_TRUE(stability.ok());
  EXPECT_NEAR(stability.value, 1.0, 1e-6);
}

TEST(PoseEngine, OneOrientationIsNotEnoughToJudgeMotion) {
  // A single attitude says where the phone is, not whether it is moving. Reporting "perfectly
  // still" from it would let a burst fire mid-swing on the very first frame after a dropout.
  OrientationPoseEngine engine;
  const std::vector<ImuSample> single{Oriented(0, 0.0, 0.0)};
  EXPECT_FALSE(engine.Stability(single).ok());
}

TEST(PoseEngine, StillPrefersMeasuredRatesWhenAPlatformSuppliesThem) {
  // A device that reports real rates should be judged on them rather than on differences between
  // filtered attitudes, which are noisier in exactly the band that matters.
  OrientationPoseEngine engine;
  const std::vector<ImuSample> spun{Spinning(0, 3.0), Spinning(100'000'000, 3.0)};
  auto stability = engine.Stability(spun);
  ASSERT_TRUE(stability.ok());
  EXPECT_LT(stability.value, 0.2);
}

TEST(PoseEngine, IgnoresATimestampThatDidNotAdvance) {
  // Two samples with the same timestamp give an infinite rate if divided naively.
  OrientationPoseEngine engine;
  const std::vector<ImuSample> stuck{Oriented(1'000, 0.0, 0.0), Oriented(1'000, 90.0, 0.0)};
  auto stability = engine.Stability(stuck);
  if (stability.ok()) {
    EXPECT_TRUE(std::isfinite(stability.value));
  }
}

// --------------------------------------------------------------- fusion, where there are rates
//
// An absolute attitude bounds the drift in where the device points and says nothing about how
// fast it is turning; a gyroscope is the other way round. Neither is ground truth — `01 §1.3` is
// explicit that the sensor pose is a prior — and fusing two priors makes a better one. An engine handed both and using only one is
// throwing away the half that fixes the other's weakness — and that is what happens today, since
// `Integrate` prefers the attitude on any sample carrying one and never looks at its rate.
//
// The outputs of a filter are not knowable in advance. These are: a bias that stops being read as
// motion, noise that comes out smaller than it went in, an estimate that still ends up where the
// measurements say, and a bias estimate that does not eat real rotation.

TEST(PoseEngine, AnOrientationOnlyStreamIsUnchangedByFusion) {
  // The compatibility floor, and it is not hypothetical: OrientationOnly is every browser today.
  // A stream with no rates has nothing to fuse, so blending would only add lag to the one signal
  // there is — the reading is taken as it stands, exactly as before.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::OrientationOnly);
  // Carrying a rate the platform never measured, which is what a zeroed angularVelocity is on
  // this capability. Reading it as a measurement is the mistake this asserts against.
  state = engine.Integrate(state, std::vector<ImuSample>{Oriented(0, 0.0, 0.0)}).value;
  state = engine.Integrate(state, std::vector<ImuSample>{Oriented(100'000'000, 30.0, 0.0)}).value;
  EXPECT_LT(AngleBetween(state.pose.orientation, FromAzimuthElevation(30.0, 0.0)) * kRadToDeg,
            1e-6);
}

TEST(PoseEngine, ALearnedGyroBiasStopsDriftingTheEstimateWhenTheAttitudeDropsOut) {
  // The one that matters. A gyroscope at rest does not read zero, and integrating that offset is
  // how dead reckoning walks away from the truth — 0.02 rad/s is a bit over a degree a second,
  // and a magnetometer dropout of a second or two is ordinary indoors. While attitudes are
  // arriving the error is observable, so the engine can learn the offset and keep subtracting it
  // through the stretch where nothing corrects it.
  OrientationPoseEngine engine;
  const Vec3 bias{0.0, 0.02, 0.0};
  PoseState state = Started(engine, MotionCapability::GyroAccel);

  // Two seconds of a device held still: the attitude says so, the gyroscope disagrees by exactly
  // its bias.
  int64_t t = 0;
  for (int step = 0; step < 200; ++step) {
    state = engine.Integrate(state, std::vector<ImuSample>{Fused(t, 0.0, 0.0, bias)}).value;
    t += 10'000'000;
  }
  const Quat settled = state.pose.orientation;

  // Then the attitude stops arriving for a second, which is the only time the bias can hurt.
  for (int step = 0; step < 100; ++step) {
    ImuSample rateOnly;
    rateOnly.timestampNs = t;
    rateOnly.angularVelocity = bias;
    state = engine.Integrate(state, std::vector<ImuSample>{rateOnly}).value;
    t += 10'000'000;
  }

  // Un-subtracted, one second of 0.02 rad/s is 1.15 degrees of yaw from a device that never moved.
  EXPECT_LT(AngleBetween(state.pose.orientation, settled) * kRadToDeg, 0.2);
}

TEST(PoseEngine, AGyroscopeCountsWhateverElseTheDeviceHasBesideIt) {
  // GyroAccelMag is GyroAccel with a magnetometer on top — strictly the better-equipped device,
  // and the one whose attitude most needs a gyroscope's help, since a magnetometer is the noisy
  // half. Testing the capability for one exact value quietly excluded it, which would have given
  // the best hardware the worst behaviour.
  OrientationPoseEngine engine;
  const Vec3 bias{0.0, 0.02, 0.0};
  PoseState state = Started(engine, MotionCapability::GyroAccelMag);

  int64_t t = 0;
  for (int step = 0; step < 200; ++step) {
    state = engine.Integrate(state, std::vector<ImuSample>{Fused(t, 0.0, 0.0, bias)}).value;
    t += 10'000'000;
  }
  EXPECT_GT(state.gyroBias.y, 0.01);
}

TEST(PoseEngine, AJumpyAbsoluteReadingComesOutSmootherThanItWentIn) {
  // What fusion buys on a still device. An indoor magnetometer wanders by degrees between
  // samples; the gyroscope says the device has not moved. Preferring the attitude hands that
  // noise straight to the reticle, which then shivers around a target it is already on.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  const Vec3 still{0.0, 0.0, 0.0};

  int64_t t = 0;
  double worst = 0.0;
  for (int step = 0; step < 60; ++step) {
    // Deterministic rather than random: the property under test is that the output swings less
    // than the input, and a fixed alternation states the input swing exactly.
    const double noiseDeg = (step % 2 == 0) ? 3.0 : -3.0;
    state = engine.Integrate(state, std::vector<ImuSample>{Fused(t, noiseDeg, 0.0, still)}).value;
    if (step > 20) {   // past the settling, which is not what this measures
      worst = std::max(worst, AngleBetween(state.pose.orientation,
                                           FromAzimuthElevation(0.0, 0.0)) * kRadToDeg);
    }
    t += 16'000'000;
  }
  // The readings themselves are 3 degrees out every single sample.
  EXPECT_LT(worst, 1.5);
}

TEST(PoseEngine, TheEstimateStillArrivesWhereTheAttitudeSaysItIs) {
  // Smoothing that never converges is a slower way of being wrong, and it is the failure a
  // complementary filter fails into: turn the correction down far enough and the estimate simply
  // ignores the world. A device that turns and stays turned has to be followed there.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  const Vec3 still{0.0, 0.0, 0.0};

  int64_t t = 0;
  for (int step = 0; step < 300; ++step) {
    state = engine.Integrate(state, std::vector<ImuSample>{Fused(t, 45.0, 0.0, still)}).value;
    t += 16'000'000;
  }
  EXPECT_LT(AngleBetween(state.pose.orientation, FromAzimuthElevation(45.0, 0.0)) * kRadToDeg,
            0.5);
}

TEST(PoseEngine, ACorrectionAlwaysMovesTowardsTheReading) {
  // A quaternion and its negation are the same rotation, so the disagreement between two
  // attitudes can come out of the arithmetic as either the short way round or the long way. Taken
  // literally, a reading 160 degrees off would be corrected by turning 200 degrees the other way
  // — which moves the estimate further from the reading it was supposed to be following, and
  // charges the gyroscope's offset an error of the wrong sign while it is at it.
  //
  // Stated as the property rather than as the sign convention that produces it: a correction
  // closes the gap. There is no reading it should ever open it.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  const Vec3 still{0.0, 0.0, 0.0};

  state = engine.Integrate(state, std::vector<ImuSample>{Fused(0, 0.0, 0.0, still)}).value;
  const Quat before = state.pose.orientation;
  const Quat reading = FromAzimuthElevation(200.0, 0.0);
  const double gap = AngleBetween(before, reading);

  state = engine.Integrate(state, std::vector<ImuSample>{Fused(16'000'000, 200.0, 0.0, still)})
              .value;
  EXPECT_LT(AngleBetween(state.pose.orientation, reading), gap);
}

TEST(PoseEngine, RealRotationIsNotMistakenForBias) {
  // The way an integral term fails. If the correction is fed back into the bias without the
  // attitude agreeing that the device is still, a genuine turn is slowly absorbed as an offset —
  // and the estimate then lags every future turn by what it learned. Here the gyroscope and the
  // attitude tell the same true story, so there is no error to attribute to anything.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  const double rate = 0.5;   // rad/s about yaw, a deliberate sweep

  int64_t t = 0;
  for (int step = 0; step < 200; ++step) {
    const double seconds = static_cast<double>(t) * 1e-9;
    // Azimuth is measured about the same axis the rate turns, so the two agree by construction.
    state = engine.Integrate(state, std::vector<ImuSample>{
                                        Fused(t, seconds * rate * kRadToDeg, 0.0,
                                              Vec3{0.0, rate, 0.0})}).value;
    t += 10'000'000;
  }

  const double learned = std::sqrt(state.gyroBias.x * state.gyroBias.x +
                                   state.gyroBias.y * state.gyroBias.y +
                                   state.gyroBias.z * state.gyroBias.z);
  EXPECT_LT(learned, 0.02) << "a two-second sweep was absorbed as " << learned << " rad/s of bias";
}

TEST(PoseEngine, ALongGapBetweenFusedSamplesDoesNotRunTheBiasAway) {
  // The estimate of the rate error is the disagreement divided by the time it accumulated over,
  // and that division is what makes the update independent of how fast samples arrive. Charging
  // the disagreement *multiplied* by the gap instead is quadratic in it: at 60 Hz the two are
  // indistinguishable, and one second between fused samples turns a 0.02 rad/s offset into a
  // learned 0.4 — twenty times the truth and the wrong side of it.
  //
  // The invariant is physical and holds for any gap: a still device reporting `b` cannot support
  // an offset estimate outside [0, b]. There is no more bias available than the rate observed.
  OrientationPoseEngine engine;
  const Vec3 bias{0.0, 0.02, 0.0};
  PoseState state = Started(engine, MotionCapability::GyroAccel);

  int64_t t = 0;
  for (int step = 0; step < 6; ++step) {
    state = engine.Integrate(state, std::vector<ImuSample>{Fused(t, 0.0, 0.0, bias)}).value;
    t += 1'000'000'000;   // a second apart: a stalled capture loop, or a backgrounded tab
  }
  EXPECT_GE(state.gyroBias.y, 0.0);
  EXPECT_LE(state.gyroBias.y, bias.y * 1.001) << "learned " << state.gyroBias.y;
}

TEST(PoseEngine, ABiasLearnedAcrossAGapStillHelpsTheDropoutItIsFor) {
  // The consequence the invariant above is protecting, stated as the thing a user would notice.
  // A runaway offset is not merely inaccurate: subtracted from the next dropout it drives the
  // estimate the other way, so dead reckoning ends up further from the truth than doing nothing
  // at all would have been. Un-subtracted, one second of 0.02 rad/s is 1.15 degrees.
  OrientationPoseEngine engine;
  const Vec3 bias{0.0, 0.02, 0.0};
  PoseState state = Started(engine, MotionCapability::GyroAccel);

  int64_t t = 0;
  for (int step = 0; step < 6; ++step) {
    state = engine.Integrate(state, std::vector<ImuSample>{Fused(t, 0.0, 0.0, bias)}).value;
    t += 1'000'000'000;
  }
  const Quat settled = state.pose.orientation;

  ImuSample dropout;
  dropout.timestampNs = t + 1'000'000'000;
  dropout.angularVelocity = bias;
  state = engine.Integrate(state, std::vector<ImuSample>{dropout}).value;

  EXPECT_LT(AngleBetween(state.pose.orientation, settled) * kRadToDeg, 1.15);
}

TEST(PoseEngine, TheBiasEstimateIsPartOfTheStateTheCallerThreadsBack) {
  // Rule 4 in docs/03 §3.3: the engine is stateless per session, so anything it learns has to be
  // a value the manager carries (ADR 0016). A bias kept inside the engine would be shared by
  // every session in the process and would survive a device being put down and picked up.
  OrientationPoseEngine engine;
  PoseState fresh = Started(engine, MotionCapability::GyroAccel);
  EXPECT_EQ(fresh.gyroBias.x, 0.0);
  EXPECT_EQ(fresh.gyroBias.y, 0.0);
  EXPECT_EQ(fresh.gyroBias.z, 0.0);

  const Vec3 bias{0.0, 0.02, 0.0};
  PoseState state = fresh;
  int64_t t = 0;
  for (int step = 0; step < 200; ++step) {
    state = engine.Integrate(state, std::vector<ImuSample>{Fused(t, 0.0, 0.0, bias)}).value;
    t += 10'000'000;
  }
  EXPECT_GT(state.gyroBias.y, 0.01);
  // And the prior it was folded from is untouched, which is what "pure" means here.
  EXPECT_EQ(fresh.gyroBias.y, 0.0);
}

}  // namespace
}  // namespace sphanorama
