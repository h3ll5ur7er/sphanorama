#pragma once

#include "sphanorama/resource_access/camera_access.h"

namespace sphanorama::bridge {

// ICameraAccess backed by the camera the page already opened.
//
// Open, preview and locks are synchronous reads and writes of state the page established before
// the core was asked to begin (ADR 0014) — so the manager can plan against a real lens.
//
// There is no burst verb left to not fit: CaptureSessionManager paces a burst over
// PeekPreviewFrame, one frame per tick of the loop the page is already running (ADR 0018). What
// remains is a single frame, which the page can hold resident like everything else here — once
// the tiered frame store exists to hold it.
class BrowserCameraAccess final : public ICameraAccess {
 public:
  Result<CameraCapabilities> Open(const CameraOpenSpec& spec) override;
  Status StartPreview() override;
  Status StopPreview() override;
  Result<FrameRef> PeekPreviewFrame() override;
  Status SetLocks(bool exposure, bool whiteBalance, bool focus) override;
  Status Close() override;
};

}  // namespace sphanorama::bridge
