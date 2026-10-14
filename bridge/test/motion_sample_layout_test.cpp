// The browser motion port's flat layout, read natively. The port itself needs Emscripten, so the
// only thing that ever read these doubles was a browser whose screen angle is always 0 and whose
// samples never carry a rate — which decodes the same whether a field is read or not.

#include "resource_access/motion_sensor_access/motion_sample_layout.h"

#include <gtest/gtest.h>

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

TEST(MotionSampleLayout, NeverWritesPastTheSpanItWasGiven) {
  const std::vector<double> flat = Distinct(3);
  std::vector<ImuSample> out(2);
  EXPECT_EQ(DecodeImuSamples(flat, out), 2u);
}

}  // namespace
}  // namespace sphanorama::bridge
