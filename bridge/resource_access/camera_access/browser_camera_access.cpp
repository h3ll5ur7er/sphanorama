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
  return host_camera_open() != 0
             ? Status::Ok()
             : Fail(StatusCode::CameraUnavailable, kComponent, "no camera open");
}

Status BrowserCameraAccess::StopPreview() { return Status::Ok(); }

Result<FrameRef> BrowserCameraAccess::PeekPreviewFrame() {
  return Err<FrameRef>(StatusCode::Unsupported, kComponent,
                       "frames need the tiered frame store, which lands in Phase 1");
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
  // Reporting success while the camera kept running let CaptureSessionManager::End return Ok
  // with the indicator still lit — which a user reads, correctly, as the app still watching them.
  // Stopping a MediaStreamTrack is synchronous, so this fits the resident-port pattern (ADR 0014)
  // without needing anything to be awaited.
  host_camera_close();
  return Status::Ok();
}

}  // namespace sphanorama::bridge
