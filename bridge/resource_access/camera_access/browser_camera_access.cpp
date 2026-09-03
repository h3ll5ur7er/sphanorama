#include "resource_access/camera_access/browser_camera_access.h"

#include <emscripten/emscripten.h>

#include <string>

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
    case 5: return caps.supportsExposureLock ? 1 : 0;
    case 6: return caps.supportsWhiteBalanceLock ? 1 : 0;
    case 7: return caps.supportsFocusLock ? 1 : 0;
    default: return 0;
  }
});

// Which locks the page confirmed are actually held, read back off the track (ADR 0022).
EM_JS(int32_t, host_camera_lock, (int32_t which), {
  const locks = Module.sphHost.cameraLocks();
  switch (which) {
    case 0: return locks.exposure ? 1 : 0;
    case 1: return locks.whiteBalance ? 1 : 0;
    case 2: return locks.focus ? 1 : 0;
    default: return 0;
  }
});

// Asks the page to put the locks back. Posted and not awaited, the same trade every write-only
// port call takes (ADR 0019) — and safe here in a way that *taking* a lock is not: a burst that
// starts before its lock is applied compares candidates on brightness, while a lock released a
// few milliseconds after the burst ended affects nothing.
EM_JS(void, host_camera_release_locks, (), {
  const host = Module.sphHost;
  if (host && host.releaseCameraLocks) host.releaseCameraLocks();
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
  // What the *track* says it can do, not what this port hopes. A camera with no manual exposure
  // mode — most desktop webcams — reports false here, the client asks for no exposure lock, and
  // the burst still fires: honest and degraded beats a burst that believes it is locked.
  capabilities.supportsExposureLock = host_camera_metric(5) != 0.0;
  capabilities.supportsFocusLock = host_camera_metric(7) != 0.0;
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
    // FailedPrecondition rather than CameraUnavailable, which is what it used to say: the
    // contract suite holds every other implementation to this answer for locking before opening,
    // and a port that disagrees with the suite it is not run against is a port that will surprise
    // the first caller to switch between them.
    return Fail(StatusCode::FailedPrecondition, kComponent, "camera is not open");
  }

  // Releasing is a write, and a write is posted (ADR 0019). Nothing is waiting on it: the burst
  // that held the lock is already over, and a lock released a few milliseconds late costs
  // nothing. Answering Ok here is not a claim about the future — it is a claim that the request
  // has been made, which is all a release needs.
  if (!exposure && !whiteBalance && !focus) {
    host_camera_release_locks();
    return Status::Ok();
  }

  // Taking one is the opposite, and this asymmetry is the decision in ADR 0022. A burst's first
  // frame arrives on the very next tick, so a lock still being applied when it lands would have
  // the manager comparing candidates on brightness rather than sharpness — and believing
  // otherwise. So this does not ask; it reads what the page already confirmed is held, and the
  // client is what makes sure that happened before it armed anything.
  const bool haveExposure = host_camera_lock(0) != 0;
  const bool haveWhiteBalance = host_camera_lock(1) != 0;
  const bool haveFocus = host_camera_lock(2) != 0;

  if ((exposure && !haveExposure) || (whiteBalance && !haveWhiteBalance) ||
      (focus && !haveFocus)) {
    // Named rather than a bare refusal: which lock is missing decides whether the caller should
    // drop that one and fire anyway or give up, and it is the difference between a camera that
    // cannot do it and a client that forgot to ask.
    return Fail(StatusCode::FailedPrecondition, kComponent,
                std::string("the page has not confirmed these locks: ") +
                    (exposure && !haveExposure ? "exposure " : "") +
                    (whiteBalance && !haveWhiteBalance ? "whiteBalance " : "") +
                    (focus && !haveFocus ? "focus" : ""));
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
