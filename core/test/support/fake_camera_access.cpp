#include "support/fake_camera_access.h"

#include <algorithm>

namespace sphanorama {
namespace {
constexpr const char* kComponent = "FakeCameraAccess";
constexpr int32_t kWidth = 32;
constexpr int32_t kHeight = 24;
}  // namespace

FakeCameraAccess::FakeCameraAccess(std::shared_ptr<IFrameStoreAccess> store)
    : store_(store ? std::move(store) : std::make_shared<FakeFrameStoreAccess>(1 << 22)) {
  capabilities_.maxWidth = kWidth;
  capabilities_.maxHeight = kHeight;
  capabilities_.horizontalFovDeg = 66.0;
  capabilities_.verticalFovDeg = 50.0;
  capabilities_.supportsExposureLock = true;
  capabilities_.supportsFocusLock = true;
  capabilities_.maxBurstFps = 30.0;
}

Result<CameraCapabilities> FakeCameraAccess::Open(const CameraOpenSpec&) {
  open_ = true;
  return Ok(capabilities_);
}

Status FakeCameraAccess::StartPreview() {
  if (!open_) return Fail(StatusCode::FailedPrecondition, kComponent, "camera is not open");
  previewing_ = true;
  return Status::Ok();
}

Status FakeCameraAccess::StopPreview() {
  previewing_ = false;
  return Status::Ok();
}

Result<FrameRef> FakeCameraAccess::PeekPreviewFrame() {
  if (!previewing_) {
    return Err<FrameRef>(StatusCode::FailedPrecondition, kComponent, "preview is not running");
  }
  return store_->Allocate(kWidth, kHeight, PixelFormat::RGBA8);
}

Result<std::vector<FrameRef>> FakeCameraAccess::CaptureBurst(const BurstSpec& burst) {
  if (!open_) {
    return Err<std::vector<FrameRef>>(StatusCode::FailedPrecondition, kComponent,
                                      "camera is not open");
  }
  if (burst.frameCount <= 0) {
    return Err<std::vector<FrameRef>>(StatusCode::InvalidArgument, kComponent,
                                      "a burst needs at least one frame");
  }

  std::vector<FrameRef> frames;
  frames.reserve(static_cast<size_t>(burst.frameCount));
  for (int32_t i = 0; i < burst.frameCount; ++i) {
    auto allocated = store_->Allocate(kWidth, kHeight, PixelFormat::RGBA8);
    if (!allocated.ok()) return allocated.status;

    // Distinct content per frame, so a selection test can tell which one survived.
    auto pinned = store_->Pin(allocated.value);
    if (!pinned.ok()) return pinned.status;
    std::fill(pinned.value.begin(), pinned.value.end(), next_fill_++);
    if (auto released = store_->Release(allocated.value); !released.ok()) return released;

    frames.push_back(allocated.value);
  }
  ++burst_count_;
  return Ok(std::move(frames));
}

Status FakeCameraAccess::SetLocks(bool exposure, bool, bool) {
  if (!open_) return Fail(StatusCode::FailedPrecondition, kComponent, "camera is not open");
  exposure_locked_ = exposure;
  return Status::Ok();
}

Status FakeCameraAccess::Close() {
  open_ = false;
  previewing_ = false;
  return Status::Ok();
}

}  // namespace sphanorama
