#pragma once
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "sphanorama/resource_access/frame_store_access.h"

namespace sphanorama {

// A frame store that forwards everything and lies about one thing.
//
// `MemoryFrameStoreAccess` is the only implementation either platform has, and it refuses
// `GpuTexture` outright — a store with no GPU tier says so rather than pretending (ADR 0020). So
// the one case a residency *restore* cannot be exercised against the real store is the one where
// getting it wrong is silent: a frame that lived in a tier this build has never seen, faulted
// into the heap to be looked at, and left there.
//
// This says a frame is wherever it is told to say, and writes down every `Demote` it is asked
// for. What it does not do is pretend to hold pixels: reads go to the real store underneath, so
// a preview taken through it is a real preview of real bytes.
class RecordingFrameStoreAccess final : public IFrameStoreAccess {
 public:
  explicit RecordingFrameStoreAccess(IFrameStoreAccess& real) : real_(real) {}

  /** Report this tier for every frame, whatever the store underneath thinks. */
  void ClaimResidency(Residency tier) { claimed_ = tier; }

  const std::vector<Residency>& demotions() const { return demotions_; }

  // A store that will not let a frame go, or put it back: what a caller does with each refusal is
  // otherwise unreachable, since the real store refuses neither for a frame it holds.
  void RefuseReleases(bool refuse) { refuse_releases_ = refuse; }
  void RefuseDemotions(bool refuse) { refuse_demotions_ = refuse; }
  // A store that cannot say where a frame is, which is not the same as saying it is gone.
  void RefuseResidency(bool refuse) { refuse_residency_ = refuse; }

  Result<FrameStoreBudget> Budget() override { return real_.Budget(); }
  Result<FrameRef> Allocate(int32_t width, int32_t height, PixelFormat format) override {
    return real_.Allocate(width, height, format);
  }
  Result<std::span<uint8_t>> Pin(const FrameRef& frame) override { return real_.Pin(frame); }
  Status Release(const FrameRef& frame) override {
    if (refuse_releases_) return Fail(StatusCode::Internal, "RecordingFrameStoreAccess", "no");
    return real_.Release(frame);
  }

  Result<Residency> ResidencyOf(const FrameRef& frame) override {
    if (refuse_residency_) {
      return Err<Residency>(StatusCode::Internal, "RecordingFrameStoreAccess", "no");
    }
    if (claimed_.has_value()) return Ok(*claimed_);
    return real_.ResidencyOf(frame);
  }

  Status Demote(const FrameRef& frame, Residency target) override {
    demotions_.push_back(target);
    if (refuse_demotions_) return Fail(StatusCode::Internal, "RecordingFrameStoreAccess", "no");
    // Forwarded rather than swallowed, so a demotion the real store would refuse still refuses
    // here — the point is what was *asked* for, and a fake that always succeeded would hide a
    // caller that depends on it succeeding.
    return real_.Demote(frame, target);
  }

  Status Adopt(const FrameRef& frame) override { return real_.Adopt(frame); }
  Status Forget(const FrameRef& frame) override { return real_.Forget(frame); }
  Status Clear() override { return real_.Clear(); }
  Result<uint64_t> TierGeneration() override { return real_.TierGeneration(); }
  Result<uint64_t> ContentHash(const FrameRef& frame) override { return real_.ContentHash(frame); }

 private:
  IFrameStoreAccess& real_;
  std::optional<Residency> claimed_;
  std::vector<Residency> demotions_;
  bool refuse_releases_ = false;
  bool refuse_demotions_ = false;
  bool refuse_residency_ = false;
};

}  // namespace sphanorama
