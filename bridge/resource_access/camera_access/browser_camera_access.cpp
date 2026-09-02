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
  capabilities.supportsExposureLock = true;
  capabilities.supportsFocusLock = true;
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

Result<std::vector<FrameRef>> BrowserCameraAccess::CaptureBurst(const BurstSpec&) {
  return Err<std::vector<FrameRef>>(
      StatusCode::Unsupported, kComponent,
      "a burst takes time and cannot be made resident in advance; see ADR 0014");
}

Status BrowserCameraAccess::SetLocks(bool, bool, bool) {
  // Accepted and not yet applied: the locks matter when a burst fires, and no burst can.
  return host_camera_open() != 0
             ? Status::Ok()
             : Fail(StatusCode::CameraUnavailable, kComponent, "no camera open");
}

Status BrowserCameraAccess::Close() { return Status::Ok(); }

}  // namespace sphanorama::bridge
