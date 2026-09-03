#pragma once

#include "sphanorama/resource_access/camera_access.h"
#include "sphanorama/resource_access/frame_store_access.h"

namespace sphanorama::bridge {

// ICameraAccess backed by the camera the page already opened.
//
// Open, preview and locks are synchronous reads and writes of state the page established before
// the core was asked to begin (ADR 0014) — so the manager can plan against a real lens.
//
// There is no burst verb left to not fit: CaptureSessionManager paces a burst over
// PeekPreviewFrame, one frame per tick of the loop the page is already running (ADR 0018). What
// remains is a single frame, and the page keeps that resident like everything else here: it
// grabs one from the viewfinder and transfers the buffer to the worker, where this reads it
// (ADR 0021).
//
// It holds the frame store because a `FrameRef` is a handle into one and every implementation of
// this contract needs somewhere to put a frame — the native fake included. That is a port
// depending on a port, sanctioned narrowly in ADR 0021 rather than worked around.
class BrowserCameraAccess final : public ICameraAccess {
 public:
  explicit BrowserCameraAccess(IFrameStoreAccess& frames) : frames_(frames) {}

  Result<CameraCapabilities> Open(const CameraOpenSpec& spec) override;
  Status StartPreview() override;
  Status StopPreview() override;
  Result<FrameRef> PeekPreviewFrame() override;
  Status SetLocks(bool exposure, bool whiteBalance, bool focus) override;
  Status Close() override;

 private:
  IFrameStoreAccess& frames_;
  // Preview is a page concept — the <video> is playing whether or not the core asked — so this
  // is the core's own view of it, and it is what makes PeekPreviewFrame refusable before
  // StartPreview the way the contract suite requires of every implementation.
  bool previewing_ = false;
};

}  // namespace sphanorama::bridge
