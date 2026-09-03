#pragma once
#include "sphanorama/types.h"

namespace sphanorama {

// V9 — where camera frames come from. Exposes a lens and a latest frame, not getUserMedia, so
// that a folder of frames can stand in for a phone camera in manager tests.
//
// Every call here is a read or a write of state the device already holds, which is what lets the
// port stay synchronous over a resident host (ADR 0014). There is deliberately no burst verb: a
// burst takes time, and the one call that took time is the one that could not be implemented.
// CaptureSessionManager paces a burst over PeekPreviewFrame instead, one frame per tick of the
// loop the client is already running (ADR 0018).
// @boundary
class ICameraAccess {
 public:
  virtual ~ICameraAccess() = default;

  virtual Result<CameraCapabilities> Open(const CameraOpenSpec& spec) = 0;
  virtual Status StartPreview() = 0;
  virtual Status StopPreview() = 0;

  // The latest frame, in the frame store; only a handle comes back. This is the whole pixel path:
  // pose correction, guidance and every frame of every burst arrive through here.
  virtual Result<FrameRef> PeekPreviewFrame() = 0;

  virtual Status SetLocks(bool exposure, bool whiteBalance, bool focus) = 0;
  virtual Status Close() = 0;
};

}  // namespace sphanorama
