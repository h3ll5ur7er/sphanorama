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
  //
  // **Each call allocates a frame and hands it over.** Not a borrow: the caller owns what comes
  // back and is the one that must Forget it. CaptureSessionManager keeps peeked frames as a
  // burst's candidates and forgets them when the burst rolls back, so a port that returned the
  // same handle twice — or one it later reclaimed — would have the manager scoring a frame whose
  // bytes had been overwritten, then forgetting a frame twice. Repeated peeks therefore return
  // distinct frames, which the contract suite asserts of every implementation.
  //
  // It follows that an implementation reaches IFrameStoreAccess, since a FrameRef is a handle
  // into it and there is nowhere else for one to come from. That is a port depending on a port,
  // which the layer rules otherwise forbid; ADR 0021 records why the frame store is the
  // exception rather than each camera being one.
  virtual Result<FrameRef> PeekPreviewFrame() = 0;

  virtual Status SetLocks(bool exposure, bool whiteBalance, bool focus) = 0;
  virtual Status Close() = 0;
};

}  // namespace sphanorama
