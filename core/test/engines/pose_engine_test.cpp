// The pose engine (V5), as far as it goes today: absolute orientation passed through, gyro rates
// integrated, stability from how fast the device is moving. Visual correction is Phase 2.
//
// A fused estimate's output is not knowable in advance, but these are: integrating nothing
// changes nothing, integrating a known rate for a known time turns by a known angle, and an
// engine that cannot estimate says so rather than guessing.
#include <gtest/gtest.h>

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

OrientationPoseEngine Started(MotionCapability capability = MotionCapability::OrientationOnly) {
  OrientationPoseEngine engine;
  EXPECT_TRUE(engine.Reset(PoseMode::Fused, capability).ok());
  return engine;
}

TEST(PoseEngine, StartsAtIdentityWithNoConfidence) {
  // Nothing has been observed yet, and a confidence of zero is how a caller learns that rather
  // than by trusting an orientation nothing produced.
  auto engine = Started();
  auto pose = engine.Integrate({});
  ASSERT_TRUE(pose.ok());
  EXPECT_NEAR(AngleBetween(pose.value.orientation, Quat{}), 0.0, 1e-12);
  EXPECT_DOUBLE_EQ(pose.value.confidence, 0.0);
}

TEST(PoseEngine, PassesAnAbsoluteOrientationStraightThrough) {
  // The browser reports a fused orientation, not a rate. Re-deriving one from it would add drift
  // to a value the platform already got right.
  auto engine = Started();
  const std::vector<ImuSample> samples{Oriented(1'000'000, 42.0, -10.0)};
  auto pose = engine.Integrate(samples);
  ASSERT_TRUE(pose.ok());
  EXPECT_NEAR(AngleBetween(pose.value.orientation, FromAzimuthElevation(42.0, -10.0)), 0.0, 1e-9);
  EXPECT_GT(pose.value.confidence, 0.9);
}

TEST(PoseEngine, TakesTheLatestOrientationFromABatch) {
  auto engine = Started();
  const std::vector<ImuSample> samples{
      Oriented(1'000'000, 10.0, 0.0),
      Oriented(2'000'000, 20.0, 0.0),
      Oriented(3'000'000, 30.0, 0.0),
  };
  auto pose = engine.Integrate(samples);
  ASSERT_TRUE(pose.ok());
  EXPECT_NEAR(AngleBetween(pose.value.orientation, FromAzimuthElevation(30.0, 0.0)), 0.0, 1e-9);
  EXPECT_EQ(pose.value.timestampNs, 3'000'000);
}

TEST(PoseEngine, RemembersTheLastOrientationAcrossEmptyBatches) {
  // The capture loop calls this every frame whether or not the sensor produced anything; losing
  // the pose on an empty batch would make the reticle flick back to centre between samples.
  auto engine = Started();
  const std::vector<ImuSample> samples{Oriented(1'000'000, 42.0, 0.0)};
  ASSERT_TRUE(engine.Integrate(samples).ok());

  auto pose = engine.Integrate({});
  ASSERT_TRUE(pose.ok());
  EXPECT_NEAR(AngleBetween(pose.value.orientation, FromAzimuthElevation(42.0, 0.0)), 0.0, 1e-9);
}

TEST(PoseEngine, IntegratesAKnownRateOverAKnownTime) {
  // A quarter turn per second for one second is a quarter turn. Drift aside, this is the one
  // thing gyro integration must get right.
  auto engine = Started(MotionCapability::GyroAccel);
  const double rate = 1.5707963267948966;   // pi/2 rad/s
  const std::vector<ImuSample> samples{Spinning(0, rate), Spinning(1'000'000'000, rate)};

  auto pose = engine.Integrate(samples);
  ASSERT_TRUE(pose.ok());
  EXPECT_NEAR(AngleBetween(pose.value.orientation, Quat{}) * kRadToDeg, 90.0, 0.5);
}

TEST(PoseEngine, IntegratingZeroRateChangesNothing) {
  auto engine = Started(MotionCapability::GyroAccel);
  const std::vector<ImuSample> samples{Spinning(0, 0.0), Spinning(500'000'000, 0.0)};
  auto pose = engine.Integrate(samples);
  ASSERT_TRUE(pose.ok());
  EXPECT_NEAR(AngleBetween(pose.value.orientation, Quat{}), 0.0, 1e-9);
}

TEST(PoseEngine, IntegrationAccumulatesAcrossCalls) {
  auto engine = Started(MotionCapability::GyroAccel);
  const double rate = 0.7853981633974483;   // pi/4 rad/s
  for (int i = 0; i < 2; ++i) {
    const std::vector<ImuSample> samples{
        Spinning(static_cast<int64_t>(i) * 1'000'000'000, rate),
        Spinning(static_cast<int64_t>(i + 1) * 1'000'000'000, rate)};
    ASSERT_TRUE(engine.Integrate(samples).ok());
  }
  auto pose = engine.Integrate({});
  EXPECT_NEAR(AngleBetween(pose.value.orientation, Quat{}) * kRadToDeg, 90.0, 1.0);
}

TEST(PoseEngine, AnAbsoluteReadingOverridesAccumulatedDrift) {
  // Integration drifts; an absolute reading is ground truth and must win, or the two sources
  // would fight and the reticle would lag behind the phone.
  auto engine = Started();
  const std::vector<ImuSample> spun{Spinning(0, 1.0), Spinning(1'000'000'000, 1.0)};
  ASSERT_TRUE(engine.Integrate(spun).ok());

  const std::vector<ImuSample> corrected{Oriented(2'000'000'000, 0.0, 0.0)};
  auto pose = engine.Integrate(corrected);
  ASSERT_TRUE(pose.ok());
  EXPECT_NEAR(AngleBetween(pose.value.orientation, Quat{}), 0.0, 1e-9);
}

TEST(PoseEngine, ResetForgetsEverything) {
  auto engine = Started();
  const std::vector<ImuSample> samples{Oriented(1'000'000, 90.0, 0.0)};
  ASSERT_TRUE(engine.Integrate(samples).ok());
  ASSERT_TRUE(engine.Reset(PoseMode::Fused, MotionCapability::OrientationOnly).ok());

  auto pose = engine.Integrate({});
  EXPECT_NEAR(AngleBetween(pose.value.orientation, Quat{}), 0.0, 1e-12);
  EXPECT_DOUBLE_EQ(pose.value.confidence, 0.0);
}

TEST(PoseEngine, AStillDeviceIsPerfectlyStable) {
  auto engine = Started(MotionCapability::GyroAccel);
  const std::vector<ImuSample> samples{Spinning(0, 0.0), Spinning(100'000'000, 0.0)};
  auto stability = engine.Stability(samples);
  ASSERT_TRUE(stability.ok());
  EXPECT_NEAR(stability.value, 1.0, 1e-9);
}

TEST(PoseEngine, AMovingDeviceIsNot) {
  // Firing a burst mid-swing is how a cell ends up with five blurred frames and no good one.
  auto engine = Started(MotionCapability::GyroAccel);
  const std::vector<ImuSample> samples{Spinning(0, 3.0), Spinning(100'000'000, 3.0)};
  auto stability = engine.Stability(samples);
  ASSERT_TRUE(stability.ok());
  EXPECT_LT(stability.value, 0.2);
}

TEST(PoseEngine, StabilityStaysWithinItsRange) {
  auto engine = Started(MotionCapability::GyroAccel);
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
  auto engine = Started(MotionCapability::GyroAccel);
  EXPECT_FALSE(engine.Stability({}).ok());
}

TEST(PoseEngine, VisualCorrectionIsNotAttemptedYet) {
  auto engine = Started();
  PoseSample prior;
  prior.orientation = FromAzimuthElevation(15.0, 0.0);
  auto corrected = engine.Correct(FrameRef{}, FrameRef{}, prior);
  ASSERT_TRUE(corrected.ok());
  EXPECT_FALSE(corrected.value.visuallyCorrected);
  EXPECT_NEAR(AngleBetween(corrected.value.orientation, prior.orientation), 0.0, 1e-12);
}

}  // namespace
}  // namespace sphanorama
