// The camera contract. Ordering is the whole of it: a capture session drives open -> preview ->
// burst -> close, and every implementation has to refuse the same out-of-order calls rather than
// returning something plausible.
#include <gtest/gtest.h>

#include <memory>
#include <set>

#include "sphanorama/resource_access/camera_access.h"
#include "support/fake_camera_access.h"

namespace sphanorama {
namespace {

template <typename Factory>
class CameraAccessContract : public ::testing::Test {
 protected:
  std::unique_ptr<ICameraAccess> camera = Factory::Create();

  void Open() { ASSERT_TRUE(camera->Open(CameraOpenSpec{}).ok()); }
};

using Implementations = ::testing::Types<FakeCameraAccessFactory>;
TYPED_TEST_SUITE(CameraAccessContract, Implementations);

TYPED_TEST(CameraAccessContract, OpenReportsWhatTheLensCanDo) {
  auto caps = this->camera->Open(CameraOpenSpec{});
  ASSERT_TRUE(caps.ok());
  EXPECT_GT(caps.value.maxWidth, 0);
  EXPECT_GT(caps.value.maxHeight, 0);
}

TYPED_TEST(CameraAccessContract, PreviewBeforeOpenIsRefused) {
  EXPECT_EQ(this->camera->StartPreview().code, StatusCode::FailedPrecondition);
}

TYPED_TEST(CameraAccessContract, PeekBeforeOpenIsRefused) {
  EXPECT_EQ(this->camera->PeekPreviewFrame().status.code, StatusCode::FailedPrecondition);
}

TYPED_TEST(CameraAccessContract, PeekBeforePreviewIsRefused) {
  this->Open();
  EXPECT_EQ(this->camera->PeekPreviewFrame().status.code, StatusCode::FailedPrecondition);
}

TYPED_TEST(CameraAccessContract, PeekWorksOncePreviewIsRunning) {
  this->Open();
  ASSERT_TRUE(this->camera->StartPreview().ok());
  auto frame = this->camera->PeekPreviewFrame();
  ASSERT_TRUE(frame.ok());
  EXPECT_TRUE(frame.value.id.valid());
}

TYPED_TEST(CameraAccessContract, RepeatedPeeksReturnDistinctFrames) {
  // Since ADR 0018 a burst is several peeks, so this is the property the burst rests on: peeks
  // that aliased one another would make selection meaningless while still passing every
  // count-based check the manager could make.
  this->Open();
  ASSERT_TRUE(this->camera->StartPreview().ok());
  std::set<uint64_t> ids;
  for (int i = 0; i < 4; ++i) {
    auto frame = this->camera->PeekPreviewFrame();
    ASSERT_TRUE(frame.ok()) << frame.status.detail;
    ids.insert(frame.value.id.value);
  }
  EXPECT_EQ(ids.size(), 4u);
}

TYPED_TEST(CameraAccessContract, LockingBeforeOpenIsRefused) {
  EXPECT_EQ(this->camera->SetLocks(true, true, true).code, StatusCode::FailedPrecondition);
}

TYPED_TEST(CameraAccessContract, ClosingReturnsTheCameraToItsInitialState) {
  this->Open();
  ASSERT_TRUE(this->camera->Close().ok());
  EXPECT_EQ(this->camera->PeekPreviewFrame().status.code, StatusCode::FailedPrecondition);
}

// Not part of the shared suite: only the fake exposes what the session asked it to do.
TEST(FakeCamera, RecordsThatTheSessionLockedExposureForABurst) {
  FakeCameraAccess camera;
  ASSERT_TRUE(camera.Open(CameraOpenSpec{}).ok());
  ASSERT_TRUE(camera.SetLocks(true, true, true).ok());
  EXPECT_TRUE(camera.ExposureLocked());
}

TEST(FakeCamera, PeekedFramesCarryDistinctPixelsSoSelectionIsObservable) {
  FakeCameraAccess camera;
  ASSERT_TRUE(camera.Open(CameraOpenSpec{}).ok());
  ASSERT_TRUE(camera.StartPreview().ok());
  std::vector<FrameRef> taken;
  for (int i = 0; i < 3; ++i) {
    auto frame = camera.PeekPreviewFrame();
    ASSERT_TRUE(frame.ok());
    taken.push_back(frame.value);
  }

  std::set<uint64_t> hashes;
  for (const auto& frame : taken) {
    auto hash = camera.store()->ContentHash(frame);
    ASSERT_TRUE(hash.ok());
    hashes.insert(hash.value);
  }
  EXPECT_EQ(hashes.size(), 3u);
}

}  // namespace
}  // namespace sphanorama
