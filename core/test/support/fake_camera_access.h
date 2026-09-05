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
  /**
   * How many times the camera has been asked for, which is not the same question as whether it
   * is open now: opening is what lights the indicator and raises the permission prompt, and a
   * path that opens and then closes on its way to failing has already done both.
   */
  int Opens() const { return opens_; }
  bool ExposureLocked() const { return exposure_locked_; }
  const CameraCapabilities& Capabilities() const { return capabilities_; }
  void SetCapabilities(const CameraCapabilities& caps) { capabilities_ = caps; }
  /**
   * Where this camera's fills start, so two of them can produce frames a test can tell apart.
   * Every instance counts from 1 otherwise, which is right — a fresh camera in a fresh process is
   * what it models — and it means two captures write identical bytes. A test about one capture's
   * frames being written over another's needs them different, and it cannot get there by taking a
   * frame first: that allocation would step the store's identity counter, and the collision it is
   * about is the one a store that has just started produces.
   */
  void FillFrom(uint8_t value) { next_fill_ = value; }
  /** Makes releasing the locks fail, which the real port can do: applyConstraints can reject. */
  void FailUnlock(bool fail) { fail_unlock_ = fail; }
  /** Makes closing fail, which is what leaves a camera both open and possibly still locked. */
  void FailClose(bool fail) { fail_close_ = fail; }

 private:
  std::shared_ptr<IFrameStoreAccess> store_;
  CameraCapabilities capabilities_;
  bool open_ = false;
  bool previewing_ = false;
  bool exposure_locked_ = false;
  bool fail_unlock_ = false;
  bool fail_close_ = false;
  int frames_taken_ = 0;
  int opens_ = 0;
  uint8_t next_fill_ = 1;
};

struct FakeCameraAccessFactory {
  static std::unique_ptr<ICameraAccess> Create() { return std::make_unique<FakeCameraAccess>(); }
};

}  // namespace sphanorama
