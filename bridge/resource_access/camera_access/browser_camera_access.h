#pragma once

#include "sphanorama/resource_access/camera_access.h"

namespace sphanorama::bridge {

// ICameraAccess backed by the camera the page already opened.
//
// Open, preview and locks are synchronous reads and writes of state the page established before
// the core was asked to begin (ADR 0014) — so the manager can plan against a real lens.
//
// CaptureBurst is the part that does not fit the resident pattern: a burst takes time and cannot
// be made resident in advance, so it needs either Asyncify on that call path or a redesign in
// which the client drives burst timing. It refuses until that is decided with measurements.
class BrowserCameraAccess final : public ICameraAccess {
 public:
  Result<CameraCapabilities> Open(const CameraOpenSpec& spec) override;
  Status StartPreview() override;
  Status StopPreview() override;
  Result<FrameRef> PeekPreviewFrame() override;
  Result<std::vector<FrameRef>> CaptureBurst(const BurstSpec& burst) override;
  Status SetLocks(bool exposure, bool whiteBalance, bool focus) override;
  Status Close() override;
};

}  // namespace sphanorama::bridge
