#pragma once
#include <vector>
#include "sphanorama/types.h"

namespace sphanorama {

// V9 — where camera frames come from. Exposes business verbs (CaptureBurst), not getUserMedia,
// so that a folder of frames can stand in for a phone camera in manager tests.
// @boundary
class ICameraAccess {
 public:
  virtual ~ICameraAccess() = default;

  virtual Result<CameraCapabilities> Open(const CameraOpenSpec& spec) = 0;
  virtual Status StartPreview() = 0;
  virtual Status StopPreview() = 0;

  // Latest preview frame, for pose correction and guidance. Borrowed: valid until the next call.
  virtual Result<FrameRef> PeekPreviewFrame() = 0;

  // Frames land in the frame store; only handles come back.
  virtual Result<std::vector<FrameRef>> CaptureBurst(const BurstSpec& burst) = 0;

  virtual Status SetLocks(bool exposure, bool whiteBalance, bool focus) = 0;
  virtual Status Close() = 0;
};

}  // namespace sphanorama
