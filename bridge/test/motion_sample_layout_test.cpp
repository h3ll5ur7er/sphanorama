// The browser motion port's flat layout, read natively. The port needs Emscripten, and the browser
// suite's screen angle is always 0 and its samples carry no rate, so a field read from the wrong
// slot decodes the same there as the right one.

#include "resource_access/motion_sensor_access/motion_sample_layout.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <vector>

namespace sphanorama::bridge {
namespace {

// Every slot distinct, so a field read from its neighbour's slot cannot come out right.
std::vector<double> Distinct(size_t samples) {
  std::vector<double> flat(samples * kDoublesPerSample);
  for (size_t i = 0; i < flat.size(); ++i) flat[i] = static_cast<double>(i + 1);
  return flat;
}

TEST(MotionSampleLayout, ReadsEveryFieldFromItsOwnSlot) {
  // The order capture-host.ts writes, spelled out again here rather than derived, because a
  // derived one would agree with whatever the decoder does.
  const std::vector<double> flat = Distinct(1);
  std::vector<ImuSample> out(1);
  ASSERT_EQ(DecodeImuSamples(flat, out), 1u);
  const ImuSample& s = out[0];
  EXPECT_EQ(s.timestampNs, 1);
  EXPECT_TRUE(s.hasAngularVelocity);
  EXPECT_EQ(s.angularVelocity.x, 3.0);
  EXPECT_EQ(s.angularVelocity.y, 4.0);
  EXPECT_EQ(s.angularVelocity.z, 5.0);
  EXPECT_EQ(s.acceleration.x, 6.0);
  EXPECT_EQ(s.acceleration.y, 7.0);
  EXPECT_EQ(s.acceleration.z, 8.0);
  EXPECT_TRUE(s.hasMagnetometer);
  EXPECT_EQ(s.magneticField.x, 10.0);
  EXPECT_EQ(s.magneticField.y, 11.0);
  EXPECT_EQ(s.magneticField.z, 12.0);
  EXPECT_TRUE(s.hasOrientation);
  EXPECT_EQ(s.orientation.w, 14.0);
  EXPECT_EQ(s.orientation.x, 15.0);
  EXPECT_EQ(s.orientation.y, 16.0);
  EXPECT_EQ(s.orientation.z, 17.0);
  EXPECT_EQ(s.screenAngleDeg, 18.0);
}

TEST(MotionSampleLayout, StepsByTheWholeSample) {
  const std::vector<double> flat = Distinct(3);
  std::vector<ImuSample> out(3);
  ASSERT_EQ(DecodeImuSamples(flat, out), 3u);
  EXPECT_EQ(out[2].timestampNs, static_cast<int64_t>(2 * kDoublesPerSample + 1));
  EXPECT_EQ(out[2].screenAngleDeg, static_cast<double>(3 * kDoublesPerSample));
}

TEST(MotionSampleLayout, DropsASampleWhoseTimestampIsNoTimeAndKeepsTheRest) {
  std::vector<double> flat = Distinct(3);
  flat[kDoublesPerSample] = std::numeric_limits<double>::quiet_NaN();
  std::vector<ImuSample> out(3);
  ASSERT_EQ(DecodeImuSamples(flat, out), 2u);
  EXPECT_EQ(out[0].timestampNs, 1);
  EXPECT_EQ(out[1].timestampNs, static_cast<int64_t>(2 * kDoublesPerSample + 1));
}

TEST(MotionSampleLayout, ReadsEachFlagFromItsOwnSlot) {
  // Every slot above is non-zero, so every flag reads set whichever slot it is taken from. Each
  // flag cleared alone, so one read from another flag's slot cannot come out right either. A
  // zeroed rate read as a measurement is a swinging phone reported still (ADR 0025).
  for (const size_t cleared : {size_t{1}, size_t{8}, size_t{12}}) {
    std::vector<double> flat = Distinct(1);
    flat[cleared] = 0.0;
    std::vector<ImuSample> out(1);
    ASSERT_EQ(DecodeImuSamples(flat, out), 1u);
    EXPECT_EQ(out[0].hasAngularVelocity, cleared != 1) << "slot " << cleared;
    EXPECT_EQ(out[0].hasMagnetometer, cleared != 8) << "slot " << cleared;
    EXPECT_EQ(out[0].hasOrientation, cleared != 12) << "slot " << cleared;
  }
}

TEST(MotionSampleLayout, DropsATimestampTooLargeForTheClockAsWellAsOneThatIsNoNumber) {
  // Finite is not enough: a double past int64's range is undefined to convert.
  std::vector<double> flat = Distinct(2);
  flat[0] = 1e30;
  std::vector<ImuSample> out(2);
  ASSERT_EQ(DecodeImuSamples(flat, out), 1u);
  EXPECT_EQ(out[0].timestampNs, static_cast<int64_t>(kDoublesPerSample + 1));
}

TEST(MotionSampleLayout, DecodesNoMoreSamplesThanItWasHanded) {
  // The port hands a span with room for a whole drain and doubles for what the page returned. A
  // sample decoded from the room past them is one nobody measured — at screen angle 0, so in
  // landscape it would turn the estimate back into portrait at the end of every drain. Shaped as
  // the port shapes it: a zeroed buffer with room for the drain, one sample at its head, and a
  // span over that sample alone.
  std::vector<double> room(8 * kDoublesPerSample, 0.0);
  const std::vector<double> one = Distinct(1);
  std::copy(one.begin(), one.end(), room.begin());
  std::vector<ImuSample> out(8);
  EXPECT_EQ(DecodeImuSamples(std::span<const double>(room.data(), kDoublesPerSample), out), 1u);
}

TEST(MotionSampleLayout, NeverWritesPastTheSpanItWasGiven) {
  const std::vector<double> flat = Distinct(3);
  std::vector<ImuSample> out(2);
  EXPECT_EQ(DecodeImuSamples(flat, out), 2u);
}

}  // namespace
}  // namespace sphanorama::bridge
