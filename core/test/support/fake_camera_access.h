#pragma once

#include <memory>
#include <vector>

#include "sphanorama/resource_access/camera_access.h"
#include "sphanorama/resource_access/frame_store_access.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"

namespace sphanorama {

// A camera that hands out synthetic frames through a real frame store, so that a capture-session
// test exercises the same allocate/pin/spill path the phone will.
//
// Every frame is filled with a distinct value, which is what lets a selection test assert that
// the frame it picked is the frame that reaches the build. Since ADR 0018 a burst is just
// several peeks, so the fill lives in PeekPreviewFrame and one counter covers both.
class FakeCameraAccess final : public ICameraAccess {
 public:
  explicit FakeCameraAccess(std::shared_ptr<IFrameStoreAccess> store = nullptr);

  Result<CameraCapabilities> Open(const CameraOpenSpec& spec) override;
  Status StartPreview() override;
  Status StopPreview() override;
  Result<FrameRef> PeekPreviewFrame() override;
  Status SetLocks(bool exposure, bool whiteBalance, bool focus) override;
  Status Close() override;

  std::shared_ptr<IFrameStoreAccess> store() const { return store_; }

  // Test affordances.
  int FramesTaken() const { return frames_taken_; }
  bool IsOpen() const { return open_; }
  bool ExposureLocked() const { return exposure_locked_; }
  void SetCapabilities(const CameraCapabilities& caps) { capabilities_ = caps; }
  /** Makes releasing the locks fail, which the real port can do: applyConstraints can reject. */
  void FailUnlock(bool fail) { fail_unlock_ = fail; }

 private:
  std::shared_ptr<IFrameStoreAccess> store_;
  CameraCapabilities capabilities_;
  bool open_ = false;
  bool previewing_ = false;
  bool exposure_locked_ = false;
  bool fail_unlock_ = false;
  int frames_taken_ = 0;
  uint8_t next_fill_ = 1;
};

struct FakeCameraAccessFactory {
  static std::unique_ptr<ICameraAccess> Create() { return std::make_unique<FakeCameraAccess>(); }
};

}  // namespace sphanorama
