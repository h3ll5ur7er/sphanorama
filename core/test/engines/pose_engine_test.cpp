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

}  // namespace
}  // namespace sphanorama
