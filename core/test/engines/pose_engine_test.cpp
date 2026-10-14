// The pose engine (V5), as far as it goes today: absolute orientation fused with gyro rates, rates
// alone integrated, stability from how fast the device is moving. Visual correction is Phase 2.
//
// A fused estimate's output is not knowable in advance, but these are: integrating nothing
// changes nothing, integrating a known rate for a known time turns by a known angle, and an
// engine that cannot estimate says so rather than guessing.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

#include "engines/pose_engine/orientation_pose_engine.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr double kRadToDeg = 180.0 / std::numbers::pi;

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
  sample.hasAngularVelocity = true;
  return sample;
}

// A sample from a device that reports both: a fused attitude and the gyroscope underneath it.
//
// The offsets the tests below teach it are about the phone's X or Z, which a level phone turns as
// a tilt. An offset about the world's up is learned from the compass instead (ADR 0074).
ImuSample Fused(int64_t timestampNs, double azimuthDeg, double elevationDeg, const Vec3& rate) {
  ImuSample sample = Oriented(timestampNs, azimuthDeg, elevationDeg);
  sample.angularVelocity = rate;
  sample.hasAngularVelocity = true;
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
  const double rate = std::numbers::pi / 2.0;   // rad/s, a quarter turn a second
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
  const double rate = std::numbers::pi / 4.0;   // rad/s, an eighth turn a second
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

TEST(PoseEngine, ASampleThatReportsNothingEstimatesNothing) {
  // A sample carrying neither an attitude nor a measured rate contributes nothing, and Integrate
  // already knows it: every branch that could move the orientation is gated on one or the other,
  // under a comment saying "a sample that reports nothing should move nothing". It moved one thing
  // anyway — `observed` — and `observed` is how a caller learns whether the orientation in its
  // hand is a reading.
  //
  // What that costs is not academic. `CaptureSessionManager::ArmBurst` enforces the acceptance
  // cone only against a pose that was measured, which is what lets a phone with no motion sensor
  // capture at all (ADR 0041). One empty sample used to flip the state to observed with confidence
  // 0.5 and the orientation still at identity, and the manager then refused thirty-one cells of
  // thirty-two on the strength of a number nobody measured.
  OrientationPoseEngine engine;
  auto initial = engine.Initial(PoseMode::Fused, MotionCapability::GyroAccel);
  ASSERT_TRUE(initial.ok());

  ImuSample nothing;
  nothing.timestampNs = 1'000'000;
  nothing.hasOrientation = false;
  nothing.hasAngularVelocity = false;

  auto after = engine.Integrate(initial.value, std::span<const ImuSample>(&nothing, 1));
  ASSERT_TRUE(after.ok());
  EXPECT_FALSE(after.value.anchored) << "an empty sample estimates nothing";
  EXPECT_DOUBLE_EQ(after.value.pose.confidence, 0.0)
      << "confidence is derived from `estimated`, so it has to follow it down";
  // Nor did it start the clock: it measured nothing, so the next sample's interval is not measured
  // from it (ADR 0074).
  EXPECT_FALSE(after.value.observed);
  // And the orientation really did not move.
  EXPECT_NEAR(AngleBetween(after.value.pose.orientation, Quat{}), 0.0, 1e-15);

  // A real reading after it still lands, so this refuses an empty sample rather than the stream.
  ImuSample real = Oriented(2'000'000, 30.0, 0.0);
  auto seen = engine.Integrate(after.value, std::span<const ImuSample>(&real, 1));
  ASSERT_TRUE(seen.ok());
  EXPECT_TRUE(seen.value.anchored);
  EXPECT_DOUBLE_EQ(seen.value.pose.confidence, 1.0);
}

TEST(PoseEngine, AnAttitudeThatIsNotARotationIsNotAReading) {
  // The worst defect this branch has produced, and it is a sentinel spelled as an ordinary value.
  //
  // `Normalize(const Quat&)` answers a degenerate quaternion with `Quat{}` — and `Quat{}` is not a
  // null object, it is `{w=1,x=0,y=0,z=0}`, the identity, whose `Direction` is a perfectly
  // ordinary `(0,0,-1)`. So a sample claiming `hasOrientation` with a zero or NaN attitude was not
  // refused and was not marked unknown: the engine stored the identity, set `anchored`, and
  // published `confidence = 1.0`.
  //
  // Downstream that is not a wrong number, it is the failure ADR 0041 exists to stop. A reviewer
  // drove it against the shipped 32-cell plan: `Locate` answered `HoldStill` on node 13 at
  // 0.0000 degrees, `aimKnown` was true so the page closed and locked the reticle, the dwell
  // matured, `Fire` went out, and `ArmBurst` passed all three of its guards — including the
  // `confidence > 0` check kept specifically to refuse an unanchored identity, which cannot see
  // this one because `confidence` says the identity was measured. Real pixels, filed under a cell
  // picked by an accident of initialisation: sharp, well scored, and undetectable afterwards.
  //
  // `hasOrientation` is a claim, and this is the layer that decides whether to believe it.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  for (const Quat broken : {Quat{0, 0, 0, 0}, Quat{nan, 0, 0, 0}, Quat{1, nan, 0, 0},
                            Quat{inf, 0, 0, 0}, Quat{0, 0, inf, 0}}) {
    OrientationPoseEngine engine;
    auto initial = engine.Initial(PoseMode::Fused, MotionCapability::GyroAccel);
    ASSERT_TRUE(initial.ok());

    ImuSample claimed;
    claimed.timestampNs = 1'000'000;
    claimed.hasOrientation = true;
    claimed.orientation = broken;

    auto after = engine.Integrate(initial.value, std::span<const ImuSample>(&claimed, 1));
    ASSERT_TRUE(after.ok());
    EXPECT_FALSE(after.value.anchored)
        << "an attitude that is not a rotation anchored the pose, so the identity it fell back to "
           "is now a direction the core believes it measured";
    EXPECT_DOUBLE_EQ(after.value.pose.confidence, 0.0);

    // And the stream is not poisoned: a real reading after it still lands.
    ImuSample real = Oriented(2'000'000, 30.0, 0.0);
    auto seen = engine.Integrate(after.value, std::span<const ImuSample>(&real, 1));
    ASSERT_TRUE(seen.ok());
    EXPECT_TRUE(seen.value.anchored);
  }
}

TEST(PoseEngine, ARateTooLargeToSquareDoesNotTurnThePhone) {
  // The fourth predicate in `quaternion.cpp`, and the one the round that hardened the other three
  // deliberately left alone. `IsUsableVector` asks "is every component a number?" and says yes to
  // `{1e200, 0, 0}`, where `IsUsableRotation` — asking "will `Normalize` derive something from
  // this?" — would say no, because squaring overflows before the sum. The two are different
  // questions on purpose (see `Measured`/`Attitude` in the engine), so this is not a bug to fix in
  // the predicate; it is a claim about what happens downstream, and a claim is worth a test.
  //
  // What happens: `Magnitude` overflows to infinity, `Turned` takes the branch past its `<= 1e-9`
  // guard, and `FromAxisAngle` refuses a non-finite angle with the identity — so the phone does
  // not turn. Fails safe, by a guard two calls away from the predicate that let the value in.
  OrientationPoseEngine engine;
  auto initial = engine.Initial(PoseMode::Fused, MotionCapability::GyroAccel);
  ASSERT_TRUE(initial.ok());

  ImuSample real = Oriented(1'000'000, 30.0, 0.0);
  auto anchored = engine.Integrate(initial.value, std::span<const ImuSample>(&real, 1));
  ASSERT_TRUE(anchored.ok() && anchored.value.anchored);

  ImuSample huge;
  huge.timestampNs = 1'020'000'000;
  huge.hasAngularVelocity = true;
  huge.angularVelocity = Vec3{1e200, 0, 0};

  auto after = engine.Integrate(anchored.value, std::span<const ImuSample>(&huge, 1));
  ASSERT_TRUE(after.ok());
  EXPECT_TRUE(std::isfinite(after.value.pose.orientation.w) &&
              std::isfinite(after.value.pose.orientation.x) &&
              std::isfinite(after.value.pose.orientation.y) &&
              std::isfinite(after.value.pose.orientation.z))
      << "a rate too large to square produced an orientation that is not a number";
  EXPECT_NEAR(AngleBetween(after.value.pose.orientation, anchored.value.pose.orientation), 0.0,
              1e-12)
      << "a rate too large to square turned the phone";
  EXPECT_GT(AngleBetween(after.value.pose.orientation, Quat{}), 0.1)
      << "the estimate collapsed onto the identity, which is a cell the plan can name";

  // And the same value read as stability: infinity is not "still", so nothing arms on it.
  auto stability = engine.Stability(std::span<const ImuSample>(&huge, 1));
  ASSERT_TRUE(stability.ok()) << stability.status.detail;
  EXPECT_LT(stability.value, 0.2)
      << "a rate too large to square was read as a phone holding still";
}

TEST(PoseEngine, ARateThatIsNotAMeasurementDoesNotTurnThePhone) {
  // The same sentence about the other half of the sample, found by the boundary lens on the same
  // round. `Decode(ImuSample&)` checks `timestampNs` and hands `angularVelocity` through raw, and
  // a non-finite rate integrates to a non-finite quaternion — which `Normalize` then answers with
  // the identity, exactly as above. The reviewer measured the consequence: one poisoned sample
  // sends the estimate to the plan's origin at confidence 0.5 with `anchored` intact, so a cell
  // that had just been refused at 90 degrees off arms from the same aim a tick later.
  //
  // A rate nobody could have measured is not a rate, so it moves nothing — nor the clock, which
  // only a sample that measured something sets (ADR 0074).
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  for (const Vec3 rate : {Vec3{inf, 0, 0}, Vec3{0, nan, 0}, Vec3{-inf, inf, nan}}) {
    OrientationPoseEngine engine;
    auto initial = engine.Initial(PoseMode::Fused, MotionCapability::GyroAccel);
    ASSERT_TRUE(initial.ok());

    // Anchored first, so there is a real attitude for a bad rate to destroy.
    ImuSample real = Oriented(1'000'000, 30.0, 0.0);
    auto anchored = engine.Integrate(initial.value, std::span<const ImuSample>(&real, 1));
    ASSERT_TRUE(anchored.ok() && anchored.value.anchored);

    ImuSample poison;
    poison.timestampNs = 1'020'000'000;
    poison.hasAngularVelocity = true;
    poison.angularVelocity = rate;

    auto after = engine.Integrate(anchored.value, std::span<const ImuSample>(&poison, 1));
    ASSERT_TRUE(after.ok());
    EXPECT_NEAR(AngleBetween(after.value.pose.orientation, anchored.value.pose.orientation), 0.0,
                1e-12)
        << "a rate nobody could have measured turned the phone";
    // Specifically not to the identity, which is the direction the plan's first cells sit at.
    EXPECT_GT(AngleBetween(after.value.pose.orientation, Quat{}), 0.1)
        << "the estimate collapsed onto the identity, which is a cell the plan can name";
  }
}

TEST(PoseEngine, ARateOnlyStreamTurnsTheOrientationAndStillReportsNoAim) {
  // A gyroscope reports a rate, and a rate only becomes an orientation when there is an elapsed
  // time to integrate it over — which the first sample of a stream does not have. Every branch in
  // `Integrate` knows that: dead reckoning is gated on `advanced`, so the first rate-only sample
  // moves nothing at all. It still has to establish the clock the second one measures against, and
  // that is not the same thing as having seen where the camera points.
  //
  // Conflating the two put an unmeasured identity behind confidence 0.5, and every rule that keys
  // on confidence then treats straight-ahead as a measurement: the manager enforces the acceptance
  // cone against it and refuses thirty-one cells of thirty-two (ADR 0042).
  OrientationPoseEngine engine;
  auto initial = engine.Initial(PoseMode::GyroOnly, MotionCapability::GyroAccel);
  ASSERT_TRUE(initial.ok());

  ImuSample first;
  first.timestampNs = 1'000'000;
  first.hasAngularVelocity = true;
  first.angularVelocity = Vec3{0.0, 0.5, 0.0};

  auto after = engine.Integrate(initial.value, std::span<const ImuSample>(&first, 1));
  ASSERT_TRUE(after.ok());
  EXPECT_FALSE(after.value.anchored)
      << "the first rate has no elapsed time to turn into an orientation";
  EXPECT_DOUBLE_EQ(after.value.pose.confidence, 0.0);
  // Arrived, though — which is exactly what the second sample measures its interval against.
  EXPECT_TRUE(after.value.observed);
  EXPECT_NEAR(AngleBetween(after.value.pose.orientation, Quat{}), 0.0, 1e-15);

  // The second one does have a gap, so the stream starts turning — this refuses a sample that
  // could not have informed anything, not the gyroscope.
  //
  // And it still reports no aim, which is the half this test used to get wrong. Fixing the first
  // sample only moved the same defect one sample along: the orientation is now several degrees
  // from the identity and nobody has ever said where the identity was pointing, so a confidence of
  // 0.5 here would put an unmeasured heading behind the acceptance cone exactly as before —
  // measured on the shipped tessellation at zero of thirty-two cells armable, with the page in
  // aimed mode because `aimKnown` was true.
  ImuSample second = first;
  second.timestampNs = 1'020'000'000;
  auto turning = engine.Integrate(after.value, std::span<const ImuSample>(&second, 1));
  ASSERT_TRUE(turning.ok());
  EXPECT_GT(AngleBetween(turning.value.pose.orientation, Quat{}), 0.1)
      << "the second sample does have an interval, so it turns the device";
  EXPECT_FALSE(turning.value.anchored) << "turned, but from a direction nobody measured";
  EXPECT_DOUBLE_EQ(turning.value.pose.confidence, 0.0);
}

TEST(PoseEngine, ASampleThatSaidNothingDoesNotMakeTheNextReadingSomethingToCorrectTowards) {
  // The complementary filter has two modes and picks between them on whether there is an estimate
  // worth predicting from: with one, it predicts forward and takes a fraction of the way back to
  // the reading; without one, it takes the reading whole. Choosing wrongly is not a small error —
  // the correction share is `1 - exp(-dt/0.1)`, so at a 16 ms gap it moves about 15% of the way
  // and the pose sits most of a turn away from a reading it should simply have accepted.
  //
  // A still rate is the sample that says nothing about where the phone points and still starts
  // the clock, and an engine that took "a sample arrived" as "there is an estimate to predict
  // from" treats the *first real reading* after it as a correction to an identity nobody measured.
  // A blank sample cannot reach this, since it starts no clock (ADR 0074).
  OrientationPoseEngine engine;
  auto initial = engine.Initial(PoseMode::Fused, MotionCapability::GyroAccel);
  ASSERT_TRUE(initial.ok());

  const ImuSample nothing = Spinning(1'000'000, 0.0);

  auto quiet = engine.Integrate(initial.value, std::span<const ImuSample>(&nothing, 1));
  ASSERT_TRUE(quiet.ok());

  // A real fused reading 16 ms later: an attitude *and* a measured rate, which is the shape that
  // selects the predict-and-correct branch when there is something to predict from.
  ImuSample reading = Oriented(17'000'000, 30.0, 0.0);
  reading.hasAngularVelocity = true;

  auto after = engine.Integrate(quiet.value, std::span<const ImuSample>(&reading, 1));
  ASSERT_TRUE(after.ok());
  EXPECT_NEAR(AngleBetween(after.value.pose.orientation, reading.orientation) * kRadToDeg, 0.0, 1e-9)
      << "the first reading of a session is ground truth, not a correction to an unmeasured guess";
  EXPECT_DOUBLE_EQ(after.value.pose.confidence, 1.0);
}

TEST(PoseEngine, EveryPathThatFoldsInAReadingAnchorsAndNoOtherPathDoes) {
  // `anchored` is what `confidence` is derived from, and confidence is what `ArmBurst` and
  // `Locate` branch on — so this is the flag that decides whether a device is treated as knowing
  // where it points. It has to answer "does this orientation descend from a reading?", which is
  // not the same question as "has anything moved it": integrating a rate moves the orientation and
  // says nothing about where it started.
  //
  // Both halves are asserted here because both have been wrong. A reading path that forgets to
  // anchor reports a real orientation as "nothing produced this"; a dead-reckoning path that
  // anchors puts a heading nobody measured behind confidence 0.5, and `ArmBurst` then enforces the
  // acceptance cone against it — zero of thirty-two cells armable on the shipped tessellation.
  OrientationPoseEngine engine;

  // The fusion path: an attitude and a rate, arriving after an estimate already exists.
  {
    auto state = engine.Initial(PoseMode::Fused, MotionCapability::GyroAccel);
    ASSERT_TRUE(state.ok());
    ImuSample first = Oriented(0, 10.0, 0.0);
    auto seeded = engine.Integrate(state.value, std::span<const ImuSample>(&first, 1));
    ASSERT_TRUE(seeded.ok());
    ASSERT_TRUE(seeded.value.anchored);

    ImuSample fused = Oriented(16'000'000, 12.0, 0.0);
    fused.hasAngularVelocity = true;
    fused.angularVelocity = Vec3{0.0, 0.1, 0.0};
    auto corrected = engine.Integrate(seeded.value, std::span<const ImuSample>(&fused, 1));
    ASSERT_TRUE(corrected.ok());
    EXPECT_TRUE(corrected.value.anchored) << "the predict-and-correct path folded in a reading";
    EXPECT_DOUBLE_EQ(corrected.value.pose.confidence, 1.0);
  }

  // A prior that is `absolute` without being `anchored` — which no caller in this repository
  // produces, and `Integrate`'s prior is a caller's value rather than this engine's. The fusion
  // branch has to anchor for itself rather than inherit it, and it briefly did not: the deletion
  // was justified by `predictable` requiring the flag, which stopped being true when `predictable`
  // was rekeyed onto `absolute`. Without the write this comes back at confidence zero for a pose
  // the filter has just moved several degrees towards a real reading.
  {
    auto state = engine.Initial(PoseMode::Fused, MotionCapability::GyroAccel);
    ASSERT_TRUE(state.ok());
    PoseState prior = state.value;
    prior.observed = true;
    prior.absolute = true;
    prior.anchored = false;
    prior.pose.timestampNs = 0;

    ImuSample fused = Oriented(16'000'000, 12.0, 0.0);
    fused.hasAngularVelocity = true;
    fused.angularVelocity = Vec3{0.0, 0.1, 0.0};
    auto corrected = engine.Integrate(prior, std::span<const ImuSample>(&fused, 1));
    ASSERT_TRUE(corrected.ok());
    EXPECT_TRUE(corrected.value.anchored) << "a reading was folded in on this call";
    EXPECT_DOUBLE_EQ(corrected.value.pose.confidence, 1.0);
  }

  // The dead-reckoning path, which moves the orientation and must *not* anchor. A gyroscope says
  // how fast the device is turning; a stream that has never carried an attitude is turning away
  // from the identity it was born with, which is a direction nobody chose.
  {
    auto state = engine.Initial(PoseMode::GyroOnly, MotionCapability::GyroAccel);
    ASSERT_TRUE(state.ok());
    const std::vector<ImuSample> turning{Spinning(0, 0.5), Spinning(200'000'000, 0.5)};
    auto moved = engine.Integrate(state.value, turning);
    ASSERT_TRUE(moved.ok());
    EXPECT_GT(AngleBetween(moved.value.pose.orientation, Quat{}) * kRadToDeg, 1.0)
        << "the fixture needs the orientation to have actually moved";
    EXPECT_FALSE(moved.value.anchored)
        << "integrating a rate says how far, never from where";
    EXPECT_DOUBLE_EQ(moved.value.pose.confidence, 0.0);

    // And once a reading arrives, the same stream is anchored for good — including through the
    // dead-reckoning stretches after it, which is what confidence 0.5 is for.
    ImuSample reading = Oriented(400'000'000, 10.0, 0.0);
    auto read = engine.Integrate(moved.value, std::span<const ImuSample>(&reading, 1));
    ASSERT_TRUE(read.ok());
    ASSERT_TRUE(read.value.anchored);
    EXPECT_DOUBLE_EQ(read.value.pose.confidence, 1.0);

    const std::vector<ImuSample> drifting{Spinning(600'000'000, 0.5)};
    auto reckoned = engine.Integrate(read.value, drifting);
    ASSERT_TRUE(reckoned.ok());
    EXPECT_TRUE(reckoned.value.anchored) << "an anchor is not lost by integrating away from it";
    EXPECT_DOUBLE_EQ(reckoned.value.pose.confidence, 0.5) << "integrated, so not absolute";
  }
}

TEST(PoseEngine, ASampleThatDidNotAdvanceTheClockIsTakenWholeRatherThanBlendedOverZeroSeconds) {
  // Timestamps are a platform's to supply, and a platform can repeat one or hand back an older one
  // — a stream resuming after a background, two adapters interleaving, a device whose clock steps.
  // `elapsed` is a strict `>`, so both cases give it false, and the question is whether the
  // branches underneath are the right ones when there is no interval.
  //
  // They are, and this pins it: with no elapsed time there is nothing to have predicted over, so an
  // attitude is ground truth and a rate integrates nothing. The failure it guards against is the
  // arithmetic one — `seconds` of zero drives `share = 1 - exp(0) = 0`, which as a blend weight
  // would keep the *prediction* and discard the reading entirely.
  OrientationPoseEngine engine;
  auto initial = engine.Initial(PoseMode::Fused, MotionCapability::GyroAccel);
  ASSERT_TRUE(initial.ok());

  ImuSample first = Oriented(1'000'000'000, 10.0, 0.0);
  first.hasAngularVelocity = true;
  auto seeded = engine.Integrate(initial.value, std::span<const ImuSample>(&first, 1));
  ASSERT_TRUE(seeded.ok());

  // The same instant again, with a different attitude: taken whole.
  ImuSample repeated = Oriented(1'000'000'000, 40.0, 0.0);
  repeated.hasAngularVelocity = true;
  auto same = engine.Integrate(seeded.value, std::span<const ImuSample>(&repeated, 1));
  ASSERT_TRUE(same.ok());
  EXPECT_NEAR(AngleBetween(same.value.pose.orientation, repeated.orientation) * kRadToDeg, 0.0, 1e-9)
      << "a repeated timestamp has no interval, so there is nothing to correct across";

  // And a timestamp *behind* the state's: likewise, rather than an interval of negative seconds.
  ImuSample backwards = Oriented(500'000'000, 70.0, 0.0);
  backwards.hasAngularVelocity = true;
  auto older = engine.Integrate(same.value, std::span<const ImuSample>(&backwards, 1));
  ASSERT_TRUE(older.ok());
  EXPECT_NEAR(AngleBetween(older.value.pose.orientation, backwards.orientation) * kRadToDeg, 0.0,
              1e-9)
      << "a backwards timestamp must not integrate a negative interval";

  // A rate-only sample that does not advance the clock moves nothing at all, rather than turning
  // the device by a rate multiplied by zero — or by a negative.
  ImuSample stale = Spinning(200'000'000, 1.0);
  auto unmoved = engine.Integrate(older.value, std::span<const ImuSample>(&stale, 1));
  ASSERT_TRUE(unmoved.ok());
  EXPECT_NEAR(AngleBetween(unmoved.value.pose.orientation, older.value.pose.orientation) * kRadToDeg,
              0.0, 1e-9);

  // And the stale sample must not become the clock the *next* one is measured against, which is
  // the half this test originally stopped short of. Refusing a sample's own interval and then
  // adopting its timestamp fabricates the gap for its successor — one stale sample in a 60 Hz
  // stream would hand the next one a window hundreds of times too long, and the bias learned over
  // that window is charged as though the device really had been turning for it.
  EXPECT_EQ(unmoved.value.pose.timestampNs, older.value.pose.timestampNs)
      << "a sample too old to be integrated is too old to set the clock";

  ImuSample next = Spinning(1'016'000'000, 1.0);   // 16 ms after the last *accepted* sample
  auto resumed = engine.Integrate(unmoved.value, std::span<const ImuSample>(&next, 1));
  ASSERT_TRUE(resumed.ok());
  // One frame of a 1 rad/s turn is about 0.92°, not the 46° an 800 ms fabricated gap would give.
  EXPECT_NEAR(AngleBetween(resumed.value.pose.orientation, unmoved.value.pose.orientation)
                  * kRadToDeg, 0.9167, 0.01);
}

TEST(PoseEngine, TheFirstAbsoluteReadingIsGroundTruthEvenAfterDeadReckoningFromNowhere) {
  // The third route to the same 25.5°. A stream that opens with rate-only samples arrives at the
  // first attitude holding an orientation integrated from the identity the state was born with,
  // which is a direction nobody chose — and correcting a fraction of the way towards the first
  // real reading would keep most of that arbitrary origin.
  //
  // A prediction is only worth blending against a reading when it descends from a reading. That is
  // `absolute`, and it is why `predictable` asks for it rather than for "something moved this".
  OrientationPoseEngine engine;
  auto initial = engine.Initial(PoseMode::Fused, MotionCapability::GyroAccel);
  ASSERT_TRUE(initial.ok());

  const std::vector<ImuSample> spinning{Spinning(0, 0.7), Spinning(300'000'000, 0.7)};
  auto reckoned = engine.Integrate(initial.value, spinning);
  ASSERT_TRUE(reckoned.ok());
  ASSERT_GT(AngleBetween(reckoned.value.pose.orientation, Quat{}) * kRadToDeg, 1.0)
      << "the fixture needs the orientation to have been moved by dead reckoning";
  ASSERT_FALSE(reckoned.value.absolute) << "and for it to be a dead-reckoned one";
  ASSERT_FALSE(reckoned.value.anchored) << "and for it to descend from no reading at all";

  ImuSample reading = Oriented(316'000'000, 30.0, 0.0);
  reading.hasAngularVelocity = true;
  auto anchored = engine.Integrate(reckoned.value, std::span<const ImuSample>(&reading, 1));
  ASSERT_TRUE(anchored.ok());
  EXPECT_NEAR(AngleBetween(anchored.value.pose.orientation, reading.orientation) * kRadToDeg, 0.0,
              1e-9)
      << "the first reading of a session is ground truth however much dead reckoning preceded it";
  EXPECT_DOUBLE_EQ(anchored.value.pose.confidence, 1.0);

  // And once anchored, the filter does its job: the next reading is corrected towards, not taken
  // whole, so this narrows the branch rather than disabling it.
  ImuSample later = Oriented(332'000'000, 34.0, 0.0);
  later.hasAngularVelocity = true;
  later.angularVelocity = Vec3{0.0, 0.7, 0.0};
  auto blended = engine.Integrate(anchored.value, std::span<const ImuSample>(&later, 1));
  ASSERT_TRUE(blended.ok());
  EXPECT_GT(AngleBetween(blended.value.pose.orientation, later.orientation) * kRadToDeg, 1e-6)
      << "an anchored estimate is still blended with the reading rather than replaced by it";
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

TEST(PoseEngine, AStreamThatMeasuredNothingUsableIsNotPerfectlyStill) {
  // `Stability`'s two claim-checks, which had no test between them: a reviewer deleted each and
  // the whole suite stayed green, then probed what the deletion buys. Both answer **1.0** —
  // "perfectly still" — for a stream that measured nothing at all. That is the exact number the
  // `!measured` refusal below them exists to avoid producing, arriving through the door marked
  // "a rate was reported".
  //
  // `hasAngularVelocity` and `hasOrientation` are *claims* about what a platform put in the
  // sample, not guarantees. A port can set either beside a value that is not a measurement, and
  // `Measured`/`Attitude` are where that is checked — so a burst firing mid-swing is what a
  // missing check here costs, on the one signal the dwell consults before it arms.
  const double nan = std::numeric_limits<double>::quiet_NaN();
  OrientationPoseEngine engine;

  {
    // A rate that is not a number, claimed as one. `Measured` refuses it, nothing else can.
    ImuSample claimed;
    claimed.timestampNs = 0;
    claimed.hasAngularVelocity = true;
    claimed.angularVelocity = Vec3{nan, 0, 0};
    ImuSample later = claimed;
    later.timestampNs = 100'000'000;
    const std::vector<ImuSample> samples{claimed, later};

    auto stability = engine.Stability(samples);
    EXPECT_FALSE(stability.ok() && stability.value > 0.9)
        << "a rate nobody could have measured was read as a phone holding still";
    if (!stability.ok()) {
      EXPECT_EQ(stability.status.code, StatusCode::FailedPrecondition);
    }
  }

  {
    // The other door: no rates at all, and an attitude that is not a rotation. The gap between
    // two attitudes is the fallback measurement, and it cannot be taken from this one.
    const std::vector<ImuSample> samples{Oriented(0, 0.0, 0.0), [&] {
      ImuSample broken;
      broken.timestampNs = 100'000'000;
      broken.hasOrientation = true;
      broken.orientation = Quat{0, 0, 0, 0};
      return broken;
    }()};

    auto stability = engine.Stability(samples);
    EXPECT_FALSE(stability.ok() && stability.value > 0.9)
        << "an attitude that is not a rotation was read as a phone holding still";
    if (!stability.ok()) {
      EXPECT_EQ(stability.status.code, StatusCode::FailedPrecondition);
    }
  }
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
  // and a native platform can lose its attitude for a second or two. While attitudes are
  // arriving the error is observable, so the engine can learn the offset and keep subtracting it
  // through the stretch where nothing corrects it. About both of a level phone's tilt axes,
  // pitch and roll, since each is a separate share of the charge.
  for (const Vec3& bias : {Vec3{0.02, 0.0, 0.0}, Vec3{0.0, 0.0, 0.02}}) {
    OrientationPoseEngine engine;
    PoseState state = Started(engine, MotionCapability::GyroAccel);

    // Five minutes of a device held still, which is what an offset takes to learn (ADR 0074):
    // the attitude says so, the gyroscope disagrees by exactly its bias.
    int64_t t = 0;
    for (int step = 0; step < 30'000; ++step) {
      state = engine.Integrate(state, std::vector<ImuSample>{Fused(t, 0.0, 0.0, bias)}).value;
      t += 10'000'000;
    }
    const Quat settled = state.pose.orientation;

    // Then the attitude stops arriving for a second, which is where an unlearned offset shows
    // fastest.
    for (int step = 0; step < 100; ++step) {
      ImuSample rateOnly;
      rateOnly.timestampNs = t;
      rateOnly.angularVelocity = bias;
      rateOnly.hasAngularVelocity = true;
      state = engine.Integrate(state, std::vector<ImuSample>{rateOnly}).value;
      t += 10'000'000;
    }

    // Un-subtracted, one second of 0.02 rad/s is 1.15 degrees from a device that never moved.
    EXPECT_LT(AngleBetween(state.pose.orientation, settled) * kRadToDeg, 0.2)
        << "offset about " << (bias.x > 0.0 ? "X" : "Z");
  }
}

TEST(PoseEngine, ARateNobodyMeasuredIsNotIntegratedEither) {
  // The other half of the same rule, and the one the fusion path made reachable. A sample with
  // neither an attitude nor a measured rate carries a zero-filled `angularVelocity`; subtracting
  // a learned offset from it and integrating the result turns "nothing was reported" into a
  // rotation backwards at the offset's own rate.
  OrientationPoseEngine engine;
  const Vec3 bias{0.02, 0.0, 0.0};
  PoseState state = Started(engine, MotionCapability::GyroAccel);

  int64_t t = 0;
  for (int step = 0; step < 30'000; ++step) {
    state = engine.Integrate(state, std::vector<ImuSample>{Fused(t, 0.0, 0.0, bias)}).value;
    t += 10'000'000;
  }
  ASSERT_GT(state.gyroBias.x, 0.01) << "the offset was never learned, so this proves nothing";
  const Quat settled = state.pose.orientation;

  ImuSample silent;   // no attitude, no measured rate: a sample that reports nothing at all
  silent.timestampNs = t + 1'000'000'000;
  state = engine.Integrate(state, std::vector<ImuSample>{silent}).value;
  EXPECT_LT(AngleBetween(state.pose.orientation, settled) * kRadToDeg, 1e-9);
}

TEST(PoseEngine, ASingleMeasuredSampleDoesNotBlindTheRestOfTheBatch) {
  // Preferring rates was a whole-batch decision, and a batch is no longer one kind of sample. One
  // measured rate of zero at the start said "this batch has rates", which switched off the
  // attitude fallback for every sample after it — so a batch that opens still and then sweeps
  // through 90 degrees of attitudes came back perfectly still. Stability gates firing a burst.
  OrientationPoseEngine engine;
  ImuSample still = Fused(0, 0.0, 0.0, Vec3{0.0, 0.0, 0.0});
  const std::vector<ImuSample> batch{still, Oriented(100'000'000, 0.0, 0.0),
                                     Oriented(200'000'000, 90.0, 0.0)};
  auto stability = engine.Stability(batch);
  ASSERT_TRUE(stability.ok());
  EXPECT_LT(stability.value, 0.2) << "reported " << stability.value;
}

TEST(PoseEngine, ARateNobodyMeasuredIsNotFusedAgainst) {
  // A capability is a claim about the platform; `hasAngularVelocity` is a fact about the sample.
  // They can disagree — a DeviceMotionEvent that fires with a null rotationRate is a device that
  // has the API and is not reporting rates — and where they do, the sample wins. Fusing against a
  // rate nobody measured drags the estimate with a fiction and then learns an offset to cancel it.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  ImuSample unmeasured = Oriented(0, 0.0, 0.0);
  unmeasured.angularVelocity = Vec3{1.0, 0.0, 0.0};   // present in the struct, never measured
  state = engine.Integrate(state, std::vector<ImuSample>{unmeasured}).value;

  ImuSample later = Oriented(100'000'000, 30.0, 0.0);
  later.angularVelocity = Vec3{1.0, 0.0, 0.0};
  state = engine.Integrate(state, std::vector<ImuSample>{later}).value;

  // Taken as it stands, exactly as an OrientationOnly stream is.
  EXPECT_LT(AngleBetween(state.pose.orientation, FromAzimuthElevation(30.0, 0.0)) * kRadToDeg,
            1e-6);
  EXPECT_EQ(state.gyroBias.x, 0.0);
}

TEST(PoseEngine, ASwingingPhoneIsUnstableEvenWhenTheSampleAlsoCarriesAnAttitude) {
  // The half of this that a fused sample breaks. Stability told "no rate measured" from "rate
  // measured as zero" by hasOrientation, so a sample carrying both was read as having no rate —
  // and two identical attitudes then said the device was perfectly still while the gyroscope in
  // the same sample said it was being swung at 3 rad/s. Stability is what gates firing a burst,
  // so still-when-swinging is the one direction it must never be wrong in.
  OrientationPoseEngine engine;
  const Vec3 swung{0.0, 3.0, 0.0};
  const std::vector<ImuSample> samples{Fused(0, 0.0, 0.0, swung),
                                       Fused(100'000'000, 0.0, 0.0, swung)};
  auto stability = engine.Stability(samples);
  ASSERT_TRUE(stability.ok());
  EXPECT_LT(stability.value, 0.2);
}

TEST(PoseEngine, AGyroscopeCountsWhateverElseTheDeviceHasBesideIt) {
  // GyroAccelMag is GyroAccel with a magnetometer on top — strictly the better-equipped device,
  // and the one whose attitude most needs a gyroscope's help, since a magnetometer is the noisy
  // half. Testing the capability for one exact value quietly excluded it, which would have given
  // the best hardware the worst behaviour.
  OrientationPoseEngine engine;
  const Vec3 bias{0.02, 0.0, 0.0};
  PoseState state = Started(engine, MotionCapability::GyroAccelMag);

  int64_t t = 0;
  for (int step = 0; step < 30'000; ++step) {
    state = engine.Integrate(state, std::vector<ImuSample>{Fused(t, 0.0, 0.0, bias)}).value;
    t += 10'000'000;
  }
  EXPECT_GT(state.gyroBias.x, 0.01);
}

TEST(PoseEngine, AJumpyAbsoluteReadingComesOutSmootherThanItWentIn) {
  // What fusion buys on a still device. A platform's attitude wanders by degrees between
  // samples; the gyroscope says the device has not moved. Preferring the attitude hands that
  // noise straight to the reticle, which then shivers around a target it is already on.
  //
  // In tilt and in heading both, since they are corrected at different speeds. The first
  // reading is the truth, because the first is taken whole, and a heading that started three
  // degrees out would measure how slowly it is pulled back rather than how little it shivers.
  for (const bool inHeading : {false, true}) {
    OrientationPoseEngine engine;
    PoseState state = Started(engine, MotionCapability::GyroAccel);
    const Vec3 still{0.0, 0.0, 0.0};

    int64_t t = 0;
    double worst = 0.0;
    for (int step = 0; step < 60; ++step) {
      // Deterministic rather than random: the property under test is that the output swings less
      // than the input, and a fixed alternation states the input swing exactly.
      const double noiseDeg = step == 0 ? 0.0 : (step % 2 == 0) ? 3.0 : -3.0;
      const ImuSample sample = inHeading ? Fused(t, noiseDeg, 0.0, still)
                                         : Fused(t, 0.0, noiseDeg, still);
      state = engine.Integrate(state, std::vector<ImuSample>{sample}).value;
      if (step > 20) {   // past the settling, which is not what this measures
        worst = std::max(worst, AngleBetween(state.pose.orientation,
                                             FromAzimuthElevation(0.0, 0.0)) * kRadToDeg);
      }
      t += 16'000'000;
    }
    // The readings themselves are 3 degrees out every single sample.
    EXPECT_LT(worst, 1.5) << (inHeading ? "in heading" : "in tilt");
  }
}

TEST(PoseEngine, TheEstimateStillArrivesWhereTheAttitudeSaysItIs) {
  // Smoothing that never converges is a slower way of being wrong, and it is the failure a
  // complementary filter fails into: turn the correction down far enough and the estimate simply
  // ignores the world. A device that turns and stays turned has to be followed there.
  //
  // In tilt, which is corrected at the speed this asserts; the heading's own version is
  // `AHeadingTheCompassKeepsSayingIsReachedInTheEnd`. The first reading is level, because the
  // first is taken whole and a stream that opened at the target would arrive without correcting;
  // and the step is inside the 30° past which a reading is taken whole too.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  const Vec3 still{0.0, 0.0, 0.0};

  state = engine.Integrate(state, std::vector<ImuSample>{Fused(0, 0.0, 0.0, still)}).value;
  int64_t t = 16'000'000;
  for (int step = 0; step < 300; ++step) {
    state = engine.Integrate(state, std::vector<ImuSample>{Fused(t, 0.0, 20.0, still)}).value;
    t += 16'000'000;
  }
  EXPECT_LT(AngleBetween(state.pose.orientation, FromAzimuthElevation(0.0, 20.0)) * kRadToDeg,
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
  //
  // In pitch as well as in yaw, which the offset is learned from by different loops. Over minutes,
  // so two seconds of a turn absorbed is thousandths of a rad/s in pitch and hundred-thousandths in
  // yaw; the gyroscope and the attitude agree here exactly, so the bound is next to none.
  for (const bool inPitch : {false, true}) {
    OrientationPoseEngine engine;
    PoseState state = Started(engine, MotionCapability::GyroAccel);
    const double rate = 0.5;   // rad/s, a deliberate sweep

    int64_t t = 0;
    for (int step = 0; step < 200; ++step) {
      const double degrees = static_cast<double>(t) * 1e-9 * rate * kRadToDeg;
      // Azimuth turns about Y and elevation about X, the axes the rate turns about, so the two
      // agree by construction.
      const ImuSample sample = inPitch ? Fused(t, 0.0, degrees, Vec3{rate, 0.0, 0.0})
                                       : Fused(t, degrees, 0.0, Vec3{0.0, rate, 0.0});
      state = engine.Integrate(state, std::vector<ImuSample>{sample}).value;
      t += 10'000'000;
    }

    const double learned = std::sqrt(state.gyroBias.x * state.gyroBias.x +
                                     state.gyroBias.y * state.gyroBias.y +
                                     state.gyroBias.z * state.gyroBias.z);
    EXPECT_LT(learned, 1e-6) << "a two-second " << (inPitch ? "pitch" : "yaw")
                             << " sweep was absorbed as " << learned << " rad/s of bias";
  }
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
  // Swept across the gaps a prediction is made over and past them, where a reading is taken whole
  // and nothing is charged; about a tilt axis and about up, which are learned by different loops.
  OrientationPoseEngine engine;
  for (const Vec3& bias : {Vec3{0.02, 0.0, 0.0}, Vec3{0.0, 0.005, 0.0}}) {
    for (const int64_t gapMs : {16, 50, 100, 150, 200, 250, 500, 1'000, 10'000, 100'000}) {
      PoseState state = Started(engine, MotionCapability::GyroAccel);
      int64_t t = 0;
      const int64_t steps = std::max<int64_t>(12, 600'000 / gapMs);
      for (int64_t step = 0; step < steps; ++step) {
        state = engine.Integrate(state, std::vector<ImuSample>{Fused(t, 0.0, 0.0, bias)}).value;
        t += gapMs * 1'000'000;
      }
      const double learned = state.gyroBias.x + state.gyroBias.y;
      const double truth = bias.x + bias.y;
      EXPECT_GE(learned, 0.0) << "gap " << gapMs << " ms learned " << learned;
      EXPECT_LE(learned, truth * 1.001) << "gap " << gapMs << " ms learned " << learned;
    }
  }
}

TEST(PoseEngine, ABiasLearnedAcrossTheLongestGapStillHelpsTheDropoutItIsFor) {
  // The consequence the invariant above is protecting, stated as the thing a user would notice.
  // A runaway offset is not merely inaccurate: subtracted from the next dropout it drives the
  // estimate the other way, so dead reckoning ends up further from the truth than doing nothing
  // at all would have been. Un-subtracted, one second of 0.02 rad/s is 1.15 degrees. At the
  // longest gap a prediction is made across, so the window is as wide as it gets.
  OrientationPoseEngine engine;
  const Vec3 bias{0.02, 0.0, 0.0};
  PoseState state = Started(engine, MotionCapability::GyroAccel);

  int64_t t = 0;
  for (int step = 0; step < 1'200; ++step) {
    state = engine.Integrate(state, std::vector<ImuSample>{Fused(t, 0.0, 0.0, bias)}).value;
    t += 250'000'000;
  }
  ASSERT_GT(state.gyroBias.x, 0.019) << "the offset was never learned, so this proves nothing";
  const Quat settled = state.pose.orientation;

  ImuSample dropout;
  dropout.timestampNs = t + 1'000'000'000;
  dropout.angularVelocity = bias;
  dropout.hasAngularVelocity = true;
  state = engine.Integrate(state, std::vector<ImuSample>{dropout}).value;

  EXPECT_LT(AngleBetween(state.pose.orientation, settled) * kRadToDeg, 0.2);
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

  const Vec3 bias{0.02, 0.0, 0.0};
  PoseState state = fresh;
  int64_t t = 0;
  for (int step = 0; step < 30'000; ++step) {
    state = engine.Integrate(state, std::vector<ImuSample>{Fused(t, 0.0, 0.0, bias)}).value;
    t += 10'000'000;
  }
  EXPECT_GT(state.gyroBias.x, 0.01);
  // And the prior it was folded from is untouched, which is what "pure" means here.
  EXPECT_EQ(fresh.gyroBias.x, 0.0);
}

// A reading whose heading the compass has turned about the world's up (ADR 0074).
Quat ReadingTurnedBy(double headingDeg, double azimuthDeg, double elevationDeg) {
  return Multiply(FromAxisAngle(Vec3{0, 1, 0}, headingDeg / kRadToDeg),
                  FromAzimuthElevation(azimuthDeg, elevationDeg));
}

ImuSample StillWithReading(int64_t timestampNs, const Quat& reading) {
  ImuSample sample;
  sample.timestampNs = timestampNs;
  sample.hasOrientation = true;
  sample.orientation = reading;
  sample.angularVelocity = Vec3{0.0, 0.0, 0.0};
  sample.hasAngularVelocity = true;
  return sample;
}

TEST(PoseEngine, ACompassThatCreepsWhileTheGyroscopeSaysStillDoesNotDragTheEstimate) {
  // The field report: in some directions the reticle kept moving with the phone held still. A
  // two-second dwell is what fires a burst (ADR 0043), so that is the window measured here, and
  // twenty degrees of creep across it is a bent field rather than an unusual one.
  //
  // Swept over elevation because heading is a turn about the world's up, not about the phone's
  // own vertical: pitched sixty degrees up, most of a compass error lies along the axis the camera
  // looks down, and splitting the correction by the phone's axes would let that part through.
  for (const double elevationDeg : {0.0, 60.0, -45.0}) {
    OrientationPoseEngine engine;
    PoseState state = Started(engine, MotionCapability::GyroAccel);
    const Quat truth = FromAzimuthElevation(0.0, elevationDeg);
    int64_t t = 0;
    for (int step = 0; step <= 200; ++step) {
      const double creepDeg = 20.0 * step / 200.0;
      state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(
                                          t, ReadingTurnedBy(creepDeg, 0.0, elevationDeg))})
                  .value;
      t += 10'000'000;
    }
    EXPECT_LT(AngleBetween(state.pose.orientation, truth) * kRadToDeg, 3.0)
        << "at elevation " << elevationDeg;
  }
}

TEST(PoseEngine, ACreepingCompassTeachesTheGyroscopesOffsetAlmostNothing) {
  // The other route the creep takes to the reticle. Charged in half a second, as ADR 0024 did, a
  // compass turning at ten degrees a second is a gyroscope that reads ten degrees a second too low
  // — and the next dropout then turns a still phone at that rate, with nothing left to correct it.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  int64_t t = 0;
  for (int step = 0; step <= 200; ++step) {
    state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(
                                        t, ReadingTurnedBy(20.0 * step / 200.0, 0.0, 0.0))})
                .value;
    t += 10'000'000;
  }
  const Quat before = state.pose.orientation;
  for (int step = 0; step < 100; ++step) {
    ImuSample still;
    still.timestampNs = t;
    still.angularVelocity = Vec3{0.0, 0.0, 0.0};
    still.hasAngularVelocity = true;
    state = engine.Integrate(state, std::vector<ImuSample>{still}).value;
    t += 10'000'000;
  }
  EXPECT_LT(AngleBetween(state.pose.orientation, before) * kRadToDeg, 0.1);
}

TEST(PoseEngine, ATiltStepIsFollowedWithinHalfASecond) {
  // What must not be given up for the heading. Gravity is the half of an attitude a platform
  // measures well, and a reticle that took ten seconds to notice the phone tipping would be the
  // lag ADR 0024 set its correction time to avoid. Pitch from an attitude that is neither level
  // nor facing the reference direction, and roll from a level one.
  //
  // Level, because a roll is only a tilt there. Pitched up, the axis the camera looks down leans
  // towards the world's up, and that share of a roll is a turn gravity cannot see — the compass
  // decides it, slowly.
  struct Step {
    Quat from;
    Quat to;
  };
  const Quat level = FromAzimuthElevation(30.0, 0.0);
  for (const Step& step :
       {Step{FromAzimuthElevation(30.0, 40.0), FromAzimuthElevation(30.0, 45.0)},
        Step{level, Multiply(level, FromAxisAngle(Vec3{0, 0, 1}, 5.0 / kRadToDeg))}}) {
    OrientationPoseEngine engine;
    PoseState state = Started(engine, MotionCapability::GyroAccel);
    state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(0, step.from)}).value;
    int64_t t = 10'000'000;
    for (int sample = 0; sample < 50; ++sample) {
      state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(t, step.to)}).value;
      t += 10'000'000;
    }
    // The filter leaves 0.025° here; a correction three times slower would leave 0.93°, and a
    // ten-second one nearly all five.
    EXPECT_LT(AngleBetween(state.pose.orientation, step.to) * kRadToDeg, 0.1);
  }
}

TEST(PoseEngine, AHeadingTheCompassKeepsSayingIsReachedInTheEnd) {
  // Slow is not never. A first session's heading has to stay on the compass for a resumed one,
  // which starts on it, to lay its cells over the first's; and a reading that holds still for
  // three minutes is not a disturbance.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(
                                      0, FromAzimuthElevation(0.0, 0.0))})
              .value;
  const Quat held = FromAzimuthElevation(10.0, 0.0);
  int64_t t = 20'000'000;
  for (int step = 0; step < 9000; ++step) {
    state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(t, held)}).value;
    t += 20'000'000;
  }
  EXPECT_LT(AngleBetween(state.pose.orientation, held) * kRadToDeg, 0.5);
}

TEST(PoseEngine, AGyroscopeOffsetAboutUpIsLearnedFromTheCompassInTheEnd) {
  // An offset about up is invisible to gravity, so on a phone that keeps its tilt only the compass
  // can see it — and with nothing learning it, a proportional loop leaves the estimate ten seconds'
  // worth of the offset away from a reading that is exactly right: 11.4 degrees at this offset. A
  // sweep round the horizon does not help, because turning about up leaves up where it was in the
  // phone's frame.
  for (const double sweepRadPerSec : {0.0, 0.5}) {
    OrientationPoseEngine engine;
    PoseState state = Started(engine, MotionCapability::GyroAccel);
    const double offset = 0.02;
    int64_t t = 0;
    Quat truth;
    for (int step = 0; step <= 30'000; ++step) {
      const double seconds = static_cast<double>(t) * 1e-9;
      truth = FromAzimuthElevation(seconds * sweepRadPerSec * kRadToDeg, 0.0);
      ImuSample sample = StillWithReading(t, truth);
      sample.angularVelocity = Vec3{0.0, sweepRadPerSec + offset, 0.0};
      state = engine.Integrate(state, std::vector<ImuSample>{sample}).value;
      t += 10'000'000;
    }
    EXPECT_LT(AngleBetween(state.pose.orientation, truth) * kRadToDeg, 0.5)
        << "sweeping at " << sweepRadPerSec << " rad/s";
    EXPECT_NEAR(state.gyroBias.y, offset, offset * 0.1)
        << "sweeping at " << sweepRadPerSec << " rad/s";
  }
}

TEST(PoseEngine, AHeadingJumpBesideASmallTiltIsClosedRatherThanCircled) {
  // A heading jump must leave gravity alone. Split any other way than into the attitude's own
  // twist and swing, a large jump carries part of a small tilt with it and turns the rest
  // sideways; the fast loop chases a tilt pointing the wrong way and charges the gyroscope for it,
  // and once gravity agrees that charge lies along up, where only the heading's slow loop sees it.
  //
  // So a second in, gravity is wherever it would have been with no jump at all, whatever the size
  // of the jump; and the heading passes the reading once, by about a tenth, on its way there.
  double gravityWithoutAJump = 0.0;
  for (const double jumpDeg : {0.0, 30.0, 90.0, 150.0, 175.0}) {
    OrientationPoseEngine engine;
    PoseState state = Started(engine, MotionCapability::GyroAccel);
    state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(
                                        0, FromAzimuthElevation(0.0, 2.0))})
                .value;
    const Quat held = FromAzimuthElevation(jumpDeg, 0.0);
    int64_t t = 10'000'000;
    double overshoot = 0.0;
    int crossings = 0;
    double previous = 0.0;
    for (int step = 0; step < 30'000; ++step) {
      state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(t, held)}).value;
      if (step == 100) {
        const Vec3 estimatedUp = Rotate(Conjugate(state.pose.orientation), Vec3{0, 1, 0});
        const Vec3 readUp = Rotate(Conjugate(held), Vec3{0, 1, 0});
        const double gravity = AngleBetweenDirections(estimatedUp, readUp) * kRadToDeg;
        if (jumpDeg == 0.0) gravityWithoutAJump = gravity;
        EXPECT_NEAR(gravity, gravityWithoutAJump, 1e-4)
            << "a second after a " << jumpDeg << " degree jump";
      }
      // The heading still to go, signed: negative while short of the reading, positive past it.
      // Gravity has settled by now, so the turn about up between the two is the heading's alone.
      Quat between = Multiply(Conjugate(held), state.pose.orientation);
      if (between.w < 0.0) between = Quat{-between.w, -between.x, -between.y, -between.z};
      const double ahead = 2.0 * std::atan2(between.y, between.w) * kRadToDeg;
      if (step >= 100) {
        overshoot = std::max(overshoot, ahead);
        if (std::abs(ahead) > 1e-6 && std::abs(previous) > 1e-6 && (ahead > 0) != (previous > 0)) {
          ++crossings;
        }
        if (std::abs(ahead) > 1e-6) previous = ahead;
      }
      t += 10'000'000;
    }
    if (jumpDeg == 0.0) continue;
    EXPECT_LT(AngleBetween(state.pose.orientation, held) * kRadToDeg, 0.5)
        << "after a " << jumpDeg << " degree jump";
    EXPECT_LT(overshoot, 0.13 * jumpDeg) << "after a " << jumpDeg << " degree jump";
    EXPECT_EQ(crossings, 1) << "after a " << jumpDeg << " degree jump";
    const double learned = std::sqrt(state.gyroBias.x * state.gyroBias.x +
                                     state.gyroBias.y * state.gyroBias.y +
                                     state.gyroBias.z * state.gyroBias.z);
    EXPECT_LT(learned, 0.002) << "after a " << jumpDeg << " degree jump";
  }
}

TEST(PoseEngine, ACompassThatErrsOneWayMoreOftenTeachesTheGyroscopeNothing) {
  // The heading's offset follows the same disagreement its correction does. Learning from a
  // clipped version of it would settle the two in different places whenever the readings are
  // lopsided — the correction at their mean, the learning nearer their median — and an offset
  // nobody has would make up the difference: 0.0072 rad/s here, from a gyroscope that reads zero.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  int64_t t = 0;
  for (int step = 0; step < 12'000; ++step) {
    const Quat reading = FromAzimuthElevation(step % 10 == 9 ? 50.0 : 0.0, 0.0);
    state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(t, reading)}).value;
    t += 10'000'000;
  }
  const double learned = std::sqrt(state.gyroBias.x * state.gyroBias.x +
                                   state.gyroBias.y * state.gyroBias.y +
                                   state.gyroBias.z * state.gyroBias.z);
  EXPECT_LT(learned, 0.001);
}

TEST(PoseEngine, AVerticalOffsetOnAPitchedPhoneIsLearnedWhole) {
  // Pitched up, an offset about the phone's own vertical is partly a turn about the world's up and
  // partly a tilt — a quarter and three quarters at sixty degrees — and the two loops that learn
  // them have to meet at the offset itself. One that learned only its own share would leave the
  // rest to carry into a dropout, about an axis the gyroscope never turned about.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  const Quat pitched = FromAzimuthElevation(0.0, 60.0);
  int64_t t = 0;
  for (int step = 0; step < 30'000; ++step) {
    ImuSample sample = StillWithReading(t, pitched);
    sample.angularVelocity = Vec3{0.0, 0.02, 0.0};
    state = engine.Integrate(state, std::vector<ImuSample>{sample}).value;
    t += 10'000'000;
  }
  EXPECT_NEAR(state.gyroBias.x, 0.0, 0.0005);
  EXPECT_NEAR(state.gyroBias.y, 0.02, 0.0005);
  EXPECT_NEAR(state.gyroBias.z, 0.0, 0.0005);
}

// A phone tipped by `degrees` about a horizontal axis that has gone `turns` of the way round.
Quat TippedAbout(double turns, double degrees) {
  const double around = 2.0 * std::numbers::pi * turns;
  return FromAxisAngle(Vec3{std::cos(around), 0.0, std::sin(around)}, degrees / kRadToDeg);
}

// The rate a gyroscope with no error reports for a phone moving along `path`, taken over an
// interval far shorter than any sample gap.
template <typename Path>
Vec3 TrueRate(const Path& path, double seconds) {
  constexpr double kStep = 1e-5;
  Quat turned = Multiply(Conjugate(path(seconds - kStep / 2)), path(seconds + kStep / 2));
  if (turned.w < 0.0) turned = Quat{-turned.w, -turned.x, -turned.y, -turned.z};
  // Small enough that sin(θ/2) is θ/2 to well past the precision this needs.
  return Vec3{2.0 * turned.x / kStep, 2.0 * turned.y / kStep, 2.0 * turned.z / kStep};
}

TEST(PoseEngine, TiltThatGoesRoundDoesNotTurnTheHeading) {
  // Turns about horizontal axes do not commute: tip a phone forward, then sideways, then back, then
  // the other way, and it has turned about the vertical. An offset learned in half a second follows
  // a tilt disagreement that goes round, each charge at right angles to an up that is itself going
  // round, so the charges add up along the phone's vertical — and the heading, trusted slowly,
  // keeps what they turn. Three ways a tilt goes round: a reading whose gravity circles on a still
  // phone, a phone that really sways with readings exactly on time, and the same sway with readings
  // a sample and a bit late.
  struct Case {
    const char* what;
    double swayDeg;
    double readingDeg;
    double lateSeconds;
  };
  for (const Case& c : {Case{"a circling reading", 0.0, 0.5, 0.0},
                        Case{"a real sway", 2.0, 0.0, 0.0},
                        Case{"a real sway read late", 2.0, 0.0, 0.02}}) {
    OrientationPoseEngine engine;
    PoseState state = Started(engine, MotionCapability::GyroAccel);
    const auto truth = [&](double seconds) { return TippedAbout(seconds, c.swayDeg); };
    double worst = 0.0;
    for (int step = 0; step < 12'000; ++step) {
      const double seconds = step * 0.01;
      ImuSample sample;
      sample.timestampNs = static_cast<int64_t>(step) * 10'000'000;
      sample.hasOrientation = true;
      sample.orientation = Multiply(truth(seconds - c.lateSeconds),
                                    TippedAbout(seconds, c.readingDeg));
      sample.angularVelocity = TrueRate(truth, seconds);
      sample.hasAngularVelocity = true;
      state = engine.Integrate(state, std::vector<ImuSample>{sample}).value;
      if (step >= 6'000) {
        worst = std::max(worst, AngleBetween(state.pose.orientation, truth(seconds)) * kRadToDeg);
      }
    }
    EXPECT_LT(worst, std::max(c.swayDeg, c.readingDeg) * 0.5 + 0.5) << c.what;
    EXPECT_LT(std::abs(state.gyroBias.y), 0.001) << c.what;
  }
}

TEST(PoseEngine, NoiseInTheReadingsTiltDoesNotTurnTheHeading) {
  // The same thing at random: tilt noise with no preferred direction still goes round, a little,
  // at every sample.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  uint64_t seed = 12345;
  const auto uniform = [&seed]() {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (static_cast<double>(seed >> 11) + 0.5) / 9007199254740992.0;
  };
  const auto gaussian = [&]() {
    return std::sqrt(-2.0 * std::log(uniform())) * std::cos(2.0 * std::numbers::pi * uniform());
  };
  double worst = 0.0;
  for (int step = 0; step < 12'000; ++step) {
    const Vec3 noise{gaussian() * 3.0 / kRadToDeg, 0.0, gaussian() * 3.0 / kRadToDeg};
    const double angle = std::sqrt(noise.x * noise.x + noise.z * noise.z);
    const Quat reading = angle > 0.0 ? FromAxisAngle(noise, angle) : Quat{};
    state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(
                                        static_cast<int64_t>(step) * 10'000'000, reading)})
                .value;
    if (step >= 6'000) {
      const Vec3 forward = Direction(state.pose.orientation);
      worst = std::max(worst, std::abs(std::atan2(forward.x, -forward.z)) * kRadToDeg);
    }
  }
  EXPECT_LT(worst, 0.5);
  EXPECT_LT(std::abs(state.gyroBias.y), 0.001);
}

TEST(PoseEngine, AReadingAfterAGapInTheSamplesIsTakenAsItStands) {
  // A gyroscope's rate is a measurement of an instant. Held across a gap nobody sampled, it says
  // nothing about how the phone turned in it — and a turn made there and corrected at the heading's
  // speed would stand tens of degrees out for seconds afterwards, charged to the gyroscope as an
  // offset it does not have. A gap is a dropout of both streams, and is treated as one.
  for (const int64_t gapMs : {260, 2'000, 50'000}) {
    OrientationPoseEngine engine;
    PoseState state = Started(engine, MotionCapability::GyroAccel);
    int64_t t = 0;
    for (int step = 0; step < 100; ++step) {
      state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(
                                          t, FromAzimuthElevation(0.0, 0.0))})
                  .value;
      t += 10'000'000;
    }
    t += gapMs * 1'000'000;
    const Quat turned = FromAzimuthElevation(90.0, 0.0);
    state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(t, turned)}).value;
    EXPECT_LT(AngleBetween(state.pose.orientation, turned) * kRadToDeg, 0.01)
        << "after a " << gapMs << " ms gap";
    EXPECT_LT(std::abs(state.gyroBias.y), 1e-6) << "after a " << gapMs << " ms gap";
  }
}

TEST(PoseEngine, OneLongGapAmongShortOnesDoesNotPassTheOffset) {
  // The offset may move by at most the rate error it saw, but only a window that matches the
  // samples either side of it keeps that true. Fused across, a gap charges the whole disagreement
  // it accumulated at one kBiasSeconds'th, so only a gap near that long can pass the offset — by
  // a sixth at twice kBiasSeconds — and only such gaps are swept. About a tilt axis and about up.
  for (const Vec3& bias : {Vec3{0.02, 0.0, 0.0}, Vec3{0.0, 0.02, 0.0}}) {
    for (const int64_t gapMs : {49'700, 100'000}) {
      OrientationPoseEngine engine;
      PoseState state = Started(engine, MotionCapability::GyroAccel);
      int64_t t = 0;
      double highest = 0.0;
      // The gap comes a minute and a half in, with the offset most of the way learned and the
      // standing error the learning leaves behind still there for the gap to charge.
      for (int step = 0; step < 20'000; ++step) {
        ImuSample sample = StillWithReading(t, Quat{});
        sample.angularVelocity = bias;
        state = engine.Integrate(state, std::vector<ImuSample>{sample}).value;
        highest = std::max(highest, (state.gyroBias.x + state.gyroBias.y) / 0.02);
        t += step == 9'000 ? gapMs * 1'000'000 : 10'000'000;
      }
      EXPECT_LE(highest, 1.001) << "gap " << gapMs << " ms, offset about "
                                << (bias.x > 0.0 ? "X" : "Y");
    }
  }
}

TEST(PoseEngine, ASilentSampleDoesNotCloseAGap) {
  // A sample that carries neither an attitude nor a rate measured nothing, and the interval the
  // next reading is predicted across starts at the last one that did. Timed from the silent one
  // instead, a turn made in the gap before it would be corrected at the heading's speed — 89.9
  // degrees out on the reading after it. Silent by what it carries, not by its flags: an attitude
  // that is not a rotation, or a rate that is not a number, measured nothing either.
  ImuSample empty;
  ImuSample brokenAttitude;
  brokenAttitude.hasOrientation = true;
  brokenAttitude.orientation = Quat{0.0, 0.0, 0.0, 0.0};
  ImuSample brokenRate;
  brokenRate.hasAngularVelocity = true;
  brokenRate.angularVelocity = Vec3{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
  for (ImuSample silent : {empty, brokenAttitude, brokenRate}) {
    OrientationPoseEngine engine;
    PoseState state = Started(engine, MotionCapability::GyroAccel);
    int64_t t = 0;
    for (int step = 0; step < 100; ++step) {
      state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(t, Quat{})}).value;
      t += 10'000'000;
    }
    t += 2'000'000'000;
    silent.timestampNs = t;
    state = engine.Integrate(state, std::vector<ImuSample>{silent}).value;
    const Quat turned = FromAzimuthElevation(90.0, 0.0);
    state =
        engine.Integrate(state, std::vector<ImuSample>{StillWithReading(t + 10'000'000, turned)})
            .value;
    EXPECT_LT(AngleBetween(state.pose.orientation, turned) * kRadToDeg, 0.01)
        << (silent.hasOrientation ? "attitude" : silent.hasAngularVelocity ? "rate" : "empty");
  }
}

TEST(PoseEngine, ASilentSampleDoesNotStartTheClock) {
  // The same rule at the start of a stream. Had a silent first sample started the clock without
  // setting it, the first rate after it would be integrated across its whole timestamp — a
  // thousand seconds of turning, here, from a sample that has no interval at all.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  const int64_t t = 1'000'000'000'000;
  ImuSample silent;
  silent.timestampNs = t;
  state = engine.Integrate(state, std::vector<ImuSample>{silent}).value;
  state = engine.Integrate(state, std::vector<ImuSample>{Spinning(t + 16'000'000, 0.5)}).value;
  EXPECT_NEAR(AngleBetween(state.pose.orientation, Quat{}), 0.0, 1e-12);
  state = engine.Integrate(state, std::vector<ImuSample>{Spinning(t + 32'000'000, 0.5)}).value;
  EXPECT_NEAR(AngleBetween(state.pose.orientation, Quat{}), 0.5 * 0.016, 1e-9);
}

TEST(PoseEngine, AReadingTensOfDegreesOutIsTakenWholeWithNothingCharged) {
  // A reading tens of degrees from a prediction while the gyroscope reads still is not a
  // measurement error to be corrected slowly: a turn the gyroscope missed moves gravity by more
  // than a real tilt error does. Fused, the share of it that leans towards the world's up would be
  // corrected at the heading's speed — 48 degrees out a second later for a quarter-turn pitched
  // thirty degrees up — and the rest would teach the gyroscope an offset. A page turn is a turn of
  // the same shape that never reaches here: the sample says which frame it is in.
  struct Case {
    const char* what;
    double elevationDeg;
    double turnDeg;
  };
  for (const Case& c : {Case{"a quarter-turn, level", 0.0, 90.0},
                        Case{"a quarter-turn, pitched up", 30.0, 90.0},
                        Case{"a half-turn, nearly level", 5.0, 180.0},
                        Case{"forty degrees, level", 0.0, 40.0}}) {
    OrientationPoseEngine engine;
    PoseState state = Started(engine, MotionCapability::GyroAccel);
    const Quat held = FromAzimuthElevation(0.0, c.elevationDeg);
    const Quat rotated = Multiply(held, FromAxisAngle(Vec3{0, 0, 1}, c.turnDeg / kRadToDeg));
    int64_t t = 0;
    for (int step = 0; step < 100; ++step) {
      state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(t, held)}).value;
      t += 10'000'000;
    }
    for (int step = 0; step < 100; ++step) {
      state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(t, rotated)}).value;
      t += 10'000'000;
    }
    EXPECT_LT(AngleBetween(state.pose.orientation, rotated) * kRadToDeg, 0.01) << c.what;
    const double learned = std::sqrt(Dot(state.gyroBias, state.gyroBias));
    EXPECT_LT(learned, 1e-4) << c.what;
  }
}

// A still phone that has learned `bias` for five minutes holding `held`, then turned to `turned`
// unseen, inside a gap `lateNs` long. Returns the worst angle from `turned` over the minute after.
double WorstAfterAMissedTurn(const Quat& held, const Quat& turned, const Vec3& bias,
                             int64_t lateNs) {
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  int64_t t = 0;
  for (int step = 0; step < 30'000; ++step) {
    ImuSample sample = StillWithReading(t, held);
    sample.angularVelocity = bias;
    state = engine.Integrate(state, std::vector<ImuSample>{sample}).value;
    t += 10'000'000;
  }
  t += lateNs;
  ImuSample sample = StillWithReading(t, turned);
  sample.angularVelocity = bias;
  state = engine.Integrate(state, std::vector<ImuSample>{sample}).value;
  double worst = AngleBetween(state.pose.orientation, turned) * kRadToDeg;
  for (int step = 0; step < 6'000; ++step) {
    t += 10'000'000;
    sample.timestampNs = t;
    state = engine.Integrate(state, std::vector<ImuSample>{sample}).value;
    worst = std::max(worst, AngleBetween(state.pose.orientation, turned) * kRadToDeg);
  }
  return worst;
}

TEST(PoseEngine, APageTurnChangesNothingButTheFrame) {
  // Turning between portrait and landscape changes the frame the attitude and the rates are
  // reported in, and nothing about the phone. So the same session reported both ways has to come
  // out the same, one answer re-expressed in the other's frame, at every sample after the turn —
  // whatever the filter was in the middle of. Recognised from the readings instead, a page turn
  // was missed whenever the estimate lagged a creeping compass by more than the tolerance: the
  // learned offset left a quarter-turn wrong, 8.7 degrees out at 0.02 rad/s, and near a pole the
  // turn itself corrected at the heading's speed.
  struct Case {
    const char* what;
    double elevationDeg;
    double screenDeg;
    int64_t lateNs;
    bool turnHasRate;
  };
  for (const Case& c : {Case{"level", 0.0, 90.0, 10'000'000, true},
                        Case{"pitched up, the other way", 30.0, -90.0, 10'000'000, true},
                        Case{"pitched down, a half-turn", -69.0, 180.0, 10'000'000, true},
                        Case{"near the zenith", 85.0, 270.0, 10'000'000, true},
                        Case{"after a stall", 0.0, 90.0, 500'000'000, true},
                        Case{"ahead of the rates", 30.0, 90.0, 10'000'000, false}}) {
    const Quat held = FromAzimuthElevation(0.0, c.elevationDeg);
    const Vec3 bias{0.02, 0.01, -0.015};   // in the phone's own frame
    // The same moment in a frame turned by `screenDeg`, as the adapter would report it.
    const auto reported = [&](int64_t t, const Quat& reading, bool hasRate, double screenDeg) {
      const Quat screen = FromAxisAngle(Vec3{0, 0, 1}, -screenDeg / kRadToDeg);
      ImuSample sample = StillWithReading(t, Multiply(reading, screen));
      sample.angularVelocity = hasRate ? Rotate(Conjugate(screen), bias) : Vec3{};
      sample.hasAngularVelocity = hasRate;
      sample.screenAngleDeg = screenDeg;
      return sample;
    };
    OrientationPoseEngine engine;
    PoseState unturned = Started(engine, MotionCapability::GyroAccel);
    PoseState turned = unturned;
    double worst = 0.0;
    double worstOffset = 0.0;
    int64_t t = 0;
    // Half a minute of learning, two seconds of the compass creeping twenty degrees about up —
    // which leaves the estimate most of that behind the reading — then the turn, and ten seconds.
    for (int step = 0; step < 4'200; ++step) {
      const double creep = std::clamp((step - 3'000) / 200.0, 0.0, 1.0) * 20.0;
      const Quat reading = Multiply(FromAxisAngle(Vec3{0, 1, 0}, creep / kRadToDeg), held);
      const bool turning = step == 3'200;
      if (turning) t += c.lateNs - 10'000'000;
      const bool hasRate = !(turning && !c.turnHasRate);
      const double screenDeg = step >= 3'200 ? c.screenDeg : 0.0;
      unturned =
          engine.Integrate(unturned, std::vector<ImuSample>{reported(t, reading, hasRate, 0.0)})
              .value;
      turned =
          engine.Integrate(turned, std::vector<ImuSample>{reported(t, reading, hasRate, screenDeg)})
              .value;
      if (step >= 3'200) {
        const Quat screen = FromAxisAngle(Vec3{0, 0, 1}, -c.screenDeg / kRadToDeg);
        worst = std::max(worst, AngleBetween(turned.pose.orientation,
                                             Multiply(unturned.pose.orientation, screen)));
        const Vec3 expected = Rotate(Conjugate(screen), unturned.gyroBias);
        const Vec3 off{turned.gyroBias.x - expected.x, turned.gyroBias.y - expected.y,
                       turned.gyroBias.z - expected.z};
        worstOffset = std::max(worstOffset, std::sqrt(Dot(off, off)));
      }
      t += 10'000'000;
    }
    EXPECT_LT(worst, 1e-6) << c.what;   // radians; about where AngleBetween stops resolving
    EXPECT_LT(worstOffset, 1e-12) << c.what;
  }
}

TEST(PoseEngine, AScreenAngleThatIsNotANumberTurnsNothing) {
  // The angle crosses from the page as a double like everything else. One that is not a number
  // names no frame, so the estimate stays in the one it was in rather than turning by NaN.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  const Quat held = FromAzimuthElevation(30.0, 10.0);
  for (int step = 0; step < 10; ++step) {
    ImuSample sample = StillWithReading(step * 10'000'000, held);
    sample.screenAngleDeg = 90.0;
    state = engine.Integrate(state, std::vector<ImuSample>{sample}).value;
  }
  ImuSample broken = Spinning(100'000'000, 0.0);
  broken.screenAngleDeg = std::numeric_limits<double>::quiet_NaN();
  state = engine.Integrate(state, std::vector<ImuSample>{broken}).value;
  EXPECT_EQ(state.screenAngleDeg, 90.0);
  EXPECT_LT(AngleBetween(state.pose.orientation, held), 1e-6);
}

TEST(PoseEngine, AnEnormousScreenAngleStillTurnsAboutTheCameraAxis) {
  // Two finite angles from the page can still differ by infinity, and the turn by that difference
  // is then `FromAxisAngle`'s to refuse: a turn by NaN would collapse onto the identity, an aim
  // nobody pointed. Whatever the angles, a page turn is a turn about the axis the camera looks
  // down, so where it looks cannot move.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  const Quat held = FromAzimuthElevation(30.0, 10.0);
  ImuSample sample = StillWithReading(0, held);
  sample.screenAngleDeg = 1e308;
  state = engine.Integrate(state, std::vector<ImuSample>{sample}).value;
  ImuSample other = Spinning(10'000'000, 0.0);
  other.screenAngleDeg = -1e308;
  state = engine.Integrate(state, std::vector<ImuSample>{other}).value;
  EXPECT_LT(AngleBetweenDirections(Direction(state.pose.orientation), Direction(held)), 1e-9);
}

TEST(PoseEngine, ATurnTheGyroscopeMissedDoesNotTurnTheOffset) {
  // A real turn faster than the prediction held across a gap shorter than the bound puts gravity
  // tens of degrees out inside fusion. The phone turned, not the frame its rates are reported in,
  // so the offset it has learned is still right where it is.
  const Quat held = FromAzimuthElevation(0.0, 0.0);
  for (const double pitchDeg : {35.0, 45.0, 90.0}) {
    const Quat turned = Multiply(held, FromAxisAngle(Vec3{1, 0, 0}, pitchDeg / kRadToDeg));
    for (const Vec3& bias : {Vec3{0.0, 0.02, 0.0}, Vec3{0.0, 0.0, 0.02}}) {
      EXPECT_LT(WorstAfterAMissedTurn(held, turned, bias, 200'000'000), 0.1)
          << pitchDeg << " degrees, offset about " << (bias.y > 0.0 ? "Y" : "Z");
    }
  }
}

TEST(PoseEngine, AStillRateAcrossAGapDoesNotCloseIt) {
  // A rate is a measurement of an instant, so one that arrives after a gap says no more about how
  // the phone turned in it than a silent sample does — measured still or not. Integrated, a still
  // rate moves nothing, and were the estimate still to descend from the reading before the gap,
  // the next reading would be corrected at the heading's speed: 89.9 degrees out at confidence 1.
  OrientationPoseEngine engine;
  PoseState state = Started(engine, MotionCapability::GyroAccel);
  int64_t t = 0;
  for (int step = 0; step < 100; ++step) {
    state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(t, Quat{})}).value;
    t += 10'000'000;
  }
  // Without a gap it is an ordinary still sample between readings, and breaks nothing.
  const PoseState held =
      engine.Integrate(state, std::vector<ImuSample>{Spinning(t, 0.0)}).value;
  EXPECT_DOUBLE_EQ(held.pose.confidence, 1.0);
  t += 2'000'000'000;
  state = engine.Integrate(state, std::vector<ImuSample>{Spinning(t, 0.0)}).value;
  EXPECT_DOUBLE_EQ(state.pose.confidence, 0.5);
  const Quat turned = FromAzimuthElevation(90.0, 0.0);
  state = engine.Integrate(state, std::vector<ImuSample>{StillWithReading(t + 10'000'000, turned)})
              .value;
  EXPECT_LT(AngleBetween(state.pose.orientation, turned) * kRadToDeg, 0.01);
}

}  // namespace
}  // namespace sphanorama
