#include "resource_access/camera_access/browser_camera_access.h"

#include <emscripten/emscripten.h>

namespace sphanorama::bridge {
namespace {

constexpr const char* kComponent = "BrowserCameraAccess";

EM_JS(int32_t, host_camera_open, (), {
  return Module.sphHost && Module.sphHost.cameraOpen() ? 1 : 0;
});

// Capabilities are read as four scalars rather than a struct: the wire codec exists for the
// facade, and reaching for it here would put a second marshalling path inside a port.
EM_JS(double, host_camera_metric, (int32_t which), {
  const caps = Module.sphHost.cameraCapabilities();
  switch (which) {
    case 0: return caps.maxWidth;
    case 1: return caps.maxHeight;
    case 2: return caps.horizontalFovDeg;
    case 3: return caps.verticalFovDeg;
    case 4: return caps.supportsTorch ? 1 : 0;
    default: return 0;
  }
});

EM_JS(void, host_camera_close, (), {
  const host = Module.sphHost;
  if (host && host.closeCamera) host.closeCamera();
});

// The resident frame's shape, or zeroes when the page has not grabbed one yet. Read before the
// allocation, because the allocation is sized from it.
EM_JS(int32_t, host_preview_metric, (int32_t which), {
  const frame = Module.sphHost.previewFrame();
  if (!frame) return 0;
  return which === 0 ? frame.width : frame.height;
});

// Copies the resident frame into the store's buffer.
//
// The one place in this port where pixels move, and it is a copy rather than a view on purpose:
// the store owns its allocation and the page owns the transferred buffer, and the next grab
// replaces the latter while the manager is still holding candidates that point at this one.
//
// `expected` is passed so the copy is bounded by what the *caller* allocated rather than by what
// the page happens to be holding: the two are read at different moments, and a frame swapped in
// between would otherwise write past the end of the span.
EM_JS(int32_t, host_preview_copy, (uint8_t* into, int32_t expected), {
  const frame = Module.sphHost.previewFrame();
  if (!frame || frame.bytes.length !== expected) return 0;
  HEAPU8.set(frame.bytes, into);
  return 1;
});

}  // namespace

Result<CameraCapabilities> BrowserCameraAccess::Open(const CameraOpenSpec&) {
  // The page opens the camera; this reports what it got. Asking the core to open one would mean
  // blocking a synchronous call on a permission prompt.
  if (host_camera_open() == 0) {
    return Err<CameraCapabilities>(StatusCode::CameraUnavailable, kComponent,
                                   "the page has not opened a camera yet");
  }

  CameraCapabilities capabilities;
  capabilities.maxWidth = static_cast<int32_t>(host_camera_metric(0));
  capabilities.maxHeight = static_cast<int32_t>(host_camera_metric(1));
  capabilities.horizontalFovDeg = host_camera_metric(2);
  capabilities.verticalFovDeg = host_camera_metric(3);
  capabilities.supportsTorch = host_camera_metric(4) != 0.0;
  // False until SetLocks below actually applies them. A burst compares candidates on sharpness,
  // which only means anything if they share an exposure — a caller told the locks are supported
  // would believe a burst was locked when nothing had been locked at all.
  capabilities.supportsExposureLock = false;
  capabilities.supportsFocusLock = false;
  return Ok(capabilities);
}

Status BrowserCameraAccess::StartPreview() {
  if (host_camera_open() == 0) {
    return Fail(StatusCode::FailedPrecondition, kComponent, "no camera open");
  }
  previewing_ = true;
  return Status::Ok();
}

Status BrowserCameraAccess::StopPreview() {
  previewing_ = false;
  return Status::Ok();
}

Result<FrameRef> BrowserCameraAccess::PeekPreviewFrame() {
  if (host_camera_open() == 0 || !previewing_) {
    return Err<FrameRef>(StatusCode::FailedPrecondition, kComponent, "preview is not running");
  }

  const int32_t width = host_preview_metric(0);
  const int32_t height = host_preview_metric(1);
  if (width <= 0 || height <= 0) {
    // The stream is open and has not produced a frame the page could grab — the first ticks
    // after enabling the camera, and a backgrounded tab. Unavailable rather than a precondition
    // failure: nothing was called out of order, there is simply nothing there yet.
    return Err<FrameRef>(StatusCode::CameraUnavailable, kComponent,
                         "the page has not grabbed a frame yet");
  }

  // RGBA8 because that is what a canvas gives back and no conversion happens on the way (ADR
  // 0021). A JPEG tier is what makes a sphere's worth of these fit, and it is not built.
  auto allocated = frames_.Allocate(width, height, PixelFormat::RGBA8);
  if (!allocated.ok()) return allocated;

  auto pinned = frames_.Pin(allocated.value);
  if (!pinned.ok()) {
    (void)frames_.Forget(allocated.value);
    return pinned.status;
  }

  const bool copied = host_preview_copy(pinned.value.data(),
                                        static_cast<int32_t>(pinned.value.size())) != 0;
  const Status released = frames_.Release(allocated.value);

  if (!copied) {
    // The page replaced the frame between the two reads above, or dropped it. Handing back an
    // allocation full of zeroes would be a black frame scored as a candidate, which every
    // count-based check would accept as a capture.
    (void)frames_.Forget(allocated.value);
    return Err<FrameRef>(StatusCode::CameraUnavailable, kComponent,
                         "the frame changed shape while it was being read");
  }
  if (!released.ok()) {
    (void)frames_.Forget(allocated.value);
    return released;
  }
  return allocated;
}


Status BrowserCameraAccess::SetLocks(bool exposure, bool whiteBalance, bool focus) {
  if (host_camera_open() == 0) {
    return Fail(StatusCode::CameraUnavailable, kComponent, "no camera open");
  }
  // Refused rather than accepted-and-ignored. Applying these needs applyConstraints on the live
  // track, which is not wired up; a silent Ok would tell CaptureSessionManager the burst it is
  // about to fire has a fixed exposure when it does not, and the cell would blend with banding
  // nobody could trace back to here.
  if (exposure || whiteBalance || focus) {
    return Fail(StatusCode::Unsupported, kComponent,
                "the page does not apply camera locks yet; capabilities report them as absent");
  }
  return Status::Ok();
}

Status BrowserCameraAccess::Close() {
  previewing_ = false;
  // Reporting success while the camera kept running let CaptureSessionManager::End return Ok
  // with the indicator still lit — which a user reads, correctly, as the app still watching them.
  // Stopping a MediaStreamTrack is synchronous, so this fits the resident-port pattern (ADR 0014)
  // without needing anything to be awaited.
  host_camera_close();
  return Status::Ok();
}

}  // namespace sphanorama::bridge
