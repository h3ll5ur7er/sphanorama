#include "support/fake_camera_access.h"

#include <algorithm>

namespace sphanorama {
namespace {
constexpr const char* kComponent = "FakeCameraAccess";
constexpr int32_t kWidth = 32;
constexpr int32_t kHeight = 24;
}  // namespace

FakeCameraAccess::FakeCameraAccess(std::shared_ptr<IFrameStoreAccess> store)
    : store_(store ? std::move(store) : std::make_shared<MemoryFrameStoreAccess>(1 << 22)) {
  capabilities_.maxWidth = kWidth;
  capabilities_.maxHeight = kHeight;
  capabilities_.horizontalFovDeg = 66.0;
  capabilities_.verticalFovDeg = 50.0;
  capabilities_.supportsExposureLock = true;
  capabilities_.supportsFocusLock = true;
  capabilities_.maxBurstFps = 30.0;
}

Result<CameraCapabilities> FakeCameraAccess::Open(const CameraOpenSpec&) {
  // Counted before the refusal, because the count is about what was asked of the device rather
  // than what it gave back: a refused open has already raised the prompt.
  ++opens_;
  if (fail_open_) {
    return Err<CameraCapabilities>(StatusCode::CameraUnavailable, kComponent,
                                   "no usable camera");
  }
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
  auto allocated = store_->Allocate(kWidth, kHeight, PixelFormat::RGBA8);
  if (!allocated.ok()) return allocated;

  // Distinct content per frame, so a selection test can tell which one survived.
  auto pinned = store_->Pin(allocated.value);
  if (!pinned.ok()) return pinned.status;
  std::fill(pinned.value.begin(), pinned.value.end(), next_fill_++);
  if (auto released = store_->Release(allocated.value); !released.ok()) return released;

  ++frames_taken_;
  return allocated;
}


Status FakeCameraAccess::SetLocks(bool exposure, bool, bool) {
  // A lock write can change what the camera can do, which is the premise of ADR 0045: pinning an
  // exposure long is what drops a real camera from 30 fps to 15 — the exposure specifically, which
  // is why only that flag is read here. A test asks for it with
  // `SlowToOnLock`.
  if (fps_on_lock_ > 0.0 && exposure) {
    capabilities_.maxBurstFps = fps_on_lock_;
  }
  if (!open_) return Fail(StatusCode::FailedPrecondition, kComponent, "camera is not open");
  if (fail_unlock_ && !exposure) {
    // Refused *and* left locked, which is the case worth modelling: a port that failed to unlock
    // has not half-unlocked, and a caller told the burst finished would have no reason to look.
    return Fail(StatusCode::CameraUnavailable, kComponent, "the track refused to drop its locks");
  }
  exposure_locked_ = exposure;
  return Status::Ok();
}

Status FakeCameraAccess::Close() {
  if (fail_close_) {
    // Left open on purpose. A close that failed did not half-close, and a camera still open is
    // a camera whose locks are still whatever they were.
    return Fail(StatusCode::CameraUnavailable, kComponent, "the device would not release");
  }
  open_ = false;
  previewing_ = false;
  return Status::Ok();
}

}  // namespace sphanorama
