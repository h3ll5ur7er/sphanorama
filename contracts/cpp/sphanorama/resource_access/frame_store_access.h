#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V11 — where pixel bytes live. The only component that knows how much memory exists, and the
// reason a 15 GB sphere of bursts fits on a phone.
class IFrameStoreAccess {
 public:
  virtual ~IFrameStoreAccess() = default;

  virtual Result<FrameStoreBudget> Budget() = 0;
  virtual Result<FrameRef> Allocate(int32_t width, int32_t height, PixelFormat) = 0;

  // Pin promotes to HeapPinned, faulting in from spill if needed, and guarantees the mapping
  // until Release. Engines read pixels only between Pin and Release.
  virtual Result<std::span<uint8_t>> Pin(const FrameRef&) = 0;
  virtual Status Release(const FrameRef&) = 0;

  virtual Status Demote(const FrameRef&, Residency target) = 0;
  virtual Status Forget(const FrameRef&) = 0;
  virtual Result<uint64_t> ContentHash(const FrameRef&) = 0;
};

}  // namespace sphanorama
