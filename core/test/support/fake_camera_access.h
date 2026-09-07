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
  // The contract's read, and the one tests use. It was a non-virtual `const&` accessor before
  // `ICameraAccess` grew `Capabilities()` (ADR 0045); one name, one answer, so a test cannot read
  // something the manager cannot.
  Result<CameraCapabilities> Capabilities() override {
    // Not open is not the same fact as cannot open, and conflating them is how a fake hides a
    // defect: this answered `Ok` with 32x24 for a camera nobody had opened *and* for one it had
    // closed, so every caller's ordering mistake looked like a working camera.
    if (!open_) {
      return Err<CameraCapabilities>(StatusCode::FailedPrecondition, "FakeCameraAccess",
                                     "no camera open");
    }
    // Separately refusable from `Open`, because "I cannot acquire a camera" and "I have one and
    // cannot describe it" are different states and a fake that offers only the first cannot drive
    // the manager's non-fatal-refusal path at all.
    if (fail_capabilities_) {
      return Err<CameraCapabilities>(StatusCode::Internal, "FakeCameraAccess",
                                     "the camera would not answer");
    }
    return Ok(WhatItIsDoing());
  }
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
  /**
   * Makes opening fail, which is the ordinary outcome of another app holding the camera or of a
   * user declining the prompt. The ask is still counted: a refused open has raised the prompt.
   */
  void FailOpen(bool fail) { fail_open_ = fail; }
  void FailCapabilities(bool fail) { fail_capabilities_ = fail; }
  // What a real camera does when its exposure is pinned long: the frame rate drops. Nothing in
  // this fake could change a capability after `Open` before, so ADR 0045's whole subject — a
  // capability that moves, and moves *because* of a lock write — had no arrangement in the suite,
  // and moving the re-ask above `SetLocks` left every test green.
  // The drop is undone when the lock is, which is the half a reviewer found missing: the rate was
  // dropped by a lock write and restored by nothing — not the unlock, not `Close()` — so a fake
  // reopened after a locked burst reported the slow rate with no lock held, and a second `Begin`
  // would have paced by it. A real camera goes back up when it stops holding the exposure.
  //
  // Derived rather than stored, which is the second half and came from the same lens one round
  // later. Holding the free-running rate in a second member alongside the dropped one put two
  // copies of one fact in a class whose whole job is to be predictable: `SetCapabilities` called
  // while the exposure was pinned seeded the "free-running" rate from the *dropped* one, and the
  // write that applied the drop sat above `SetLocks`' own refusal path, so a refused unlock
  // reported a camera running at 30 fps while still holding the lock. Neither is reachable from
  // the suite; both stop existing when the rate is a function of `exposure_locked_`.
  void SlowToOnLock(double fps) { fps_on_lock_ = fps; }
  /** Makes releasing the locks fail, which the real port can do: applyConstraints can reject. */
  void FailUnlock(bool fail) { fail_unlock_ = fail; }
  /** Makes closing fail, which is what leaves a camera both open and possibly still locked. */
  void FailClose(bool fail) { fail_close_ = fail; }

 private:
  // What this camera reports right now: what it was configured with, with the exposure lock's cost
  // applied if it is holding one. One expression, evaluated at every read, so there is no second
  // copy of the rate to keep in step and no write ordering to get wrong.
  CameraCapabilities WhatItIsDoing() const {
    CameraCapabilities now = capabilities_;
    if (fps_on_lock_ > 0.0 && exposure_locked_) now.maxBurstFps = fps_on_lock_;
    return now;
  }

  std::shared_ptr<IFrameStoreAccess> store_;
  CameraCapabilities capabilities_;
  bool open_ = false;
  bool previewing_ = false;
  bool exposure_locked_ = false;
  bool fail_open_ = false;
  bool fail_capabilities_ = false;
  double fps_on_lock_ = 0.0;
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
