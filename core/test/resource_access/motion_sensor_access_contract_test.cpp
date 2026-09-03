// The motion-sensor contract. Sensor absence is a supported outcome rather than an error path,
// because on iOS it is one tap away at all times.
#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "sphanorama/resource_access/motion_sensor_access.h"
#include "support/fake_motion_sensor_access.h"

namespace sphanorama {
namespace {

template <typename Factory>
class MotionSensorAccessContract : public ::testing::Test {
 protected:
  std::unique_ptr<IMotionSensorAccess> sensor = Factory::Create();
};

using Implementations = ::testing::Types<FakeMotionSensorAccessFactory>;
TYPED_TEST_SUITE(MotionSensorAccessContract, Implementations);

TYPED_TEST(MotionSensorAccessContract, ReportsWhatItCanMeasure) {
  auto caps = this->sensor->Capabilities();
  ASSERT_TRUE(caps.ok());
  EXPECT_NE(caps.value, MotionCapability::None);
}

TYPED_TEST(MotionSensorAccessContract, DrainBeforeStartIsRefused) {
  std::vector<ImuSample> out(4);
  EXPECT_EQ(this->sensor->Drain(out).status.code, StatusCode::FailedPrecondition);
}

TYPED_TEST(MotionSensorAccessContract, DrainAfterStopIsRefused) {
  ASSERT_TRUE(this->sensor->Start(100).ok());
  ASSERT_TRUE(this->sensor->Stop().ok());
  std::vector<ImuSample> out(4);
  EXPECT_EQ(this->sensor->Drain(out).status.code, StatusCode::FailedPrecondition);
}

TYPED_TEST(MotionSensorAccessContract, DrainNeverWritesPastTheCallersBuffer) {
  ASSERT_TRUE(this->sensor->Start(100).ok());
  std::vector<ImuSample> out(2);
  auto drained = this->sensor->Drain(out);
  ASSERT_TRUE(drained.ok());
  EXPECT_LE(drained.value, 2);
}

TYPED_TEST(MotionSensorAccessContract, SamplesArriveInTimestampOrder) {
  // Pose integration assumes monotonic time; out-of-order samples would silently corrupt the
  // orientation estimate rather than fail.
  ASSERT_TRUE(this->sensor->Start(100).ok());
  std::vector<ImuSample> out(8);
  auto drained = this->sensor->Drain(out);
  ASSERT_TRUE(drained.ok());
  ASSERT_GT(drained.value, 1);
  for (int32_t i = 1; i < drained.value; ++i) {
    EXPECT_GE(out[static_cast<size_t>(i)].timestampNs, out[static_cast<size_t>(i - 1)].timestampNs);
  }
}

TYPED_TEST(MotionSensorAccessContract, DrainConsumesRatherThanRepeats) {
  ASSERT_TRUE(this->sensor->Start(100).ok());
  std::vector<ImuSample> first(4), second(4);
  auto a = this->sensor->Drain(first);
  auto b = this->sensor->Drain(second);
  ASSERT_TRUE(a.ok());
  ASSERT_TRUE(b.ok());
  ASSERT_GT(a.value, 0);
  ASSERT_GT(b.value, 0);
  EXPECT_NE(first[0].timestampNs, second[0].timestampNs);
}

TYPED_TEST(MotionSensorAccessContract, DrainingAnEmptyQueueIsNotAnError) {
  ASSERT_TRUE(this->sensor->Start(100).ok());
  std::vector<ImuSample> out(64);
  ASSERT_TRUE(this->sensor->Drain(out).ok());
  auto again = this->sensor->Drain(out);
  ASSERT_TRUE(again.ok());
  EXPECT_EQ(again.value, 0);
}

TYPED_TEST(MotionSensorAccessContract, RejectsANonsenseSampleRate) {
  EXPECT_EQ(this->sensor->Start(0).code, StatusCode::InvalidArgument);
}

// Not part of the shared suite: only the fake can be configured to have no sensors at all.
TEST(MotionSensorAbsence, StartIsRefusedWhenMotionAccessWasDeclined) {
  FakeMotionSensorAccess sensor(MotionCapability::None);
  EXPECT_EQ(sensor.Capabilities().value, MotionCapability::None);
  EXPECT_EQ(sensor.Start(100).code, StatusCode::SensorPermissionDenied);
}

}  // namespace
}  // namespace sphanorama
