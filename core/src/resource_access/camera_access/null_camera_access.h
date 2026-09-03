#pragma once

#include "sphanorama/resource_access/camera_access.h"

namespace sphanorama {

// Stands in until the browser port lands. A camera lives in JavaScript, so the core cannot
// supply one — and a stub that returned blank frames would let a capture session look like it
// was working.
class NullCameraAccess final : public ICameraAccess {
 public:
  Result<CameraCapabilities> Open(const CameraOpenSpec& spec) override;
  Status StartPreview() override;
  Status StopPreview() override;
  Result<FrameRef> PeekPreviewFrame() override;
  Result<std::vector<FrameRef>> CaptureBurst(const BurstSpec& burst) override;
  Status SetLocks(bool exposure, bool whiteBalance, bool focus) override;
  Status Close() override;
};

}  // namespace sphanorama
