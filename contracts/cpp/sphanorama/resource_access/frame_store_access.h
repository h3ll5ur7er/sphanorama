#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V11 — where pixel bytes live. The only component that knows how much memory exists, and the
// reason a 15 GB sphere of bursts fits on a phone.
//
// Not marked @boundary: this contract moves bytes through the shared heap rather than
// through marshalled values, so its TypeScript adapter is written against the shared-heap
// protocol rather than mirroring this signature. See ADR 0009.
class IFrameStoreAccess {
 public:
  virtual ~IFrameStoreAccess() = default;

  virtual Result<FrameStoreBudget> Budget() = 0;
  virtual Result<FrameRef> Allocate(int32_t width, int32_t height, PixelFormat) = 0;

  // Pin promotes to HeapPinned, faulting in from spill if needed, and guarantees the mapping
  // until Release. Engines read pixels only between Pin and Release.
  virtual Result<std::span<uint8_t>> Pin(const FrameRef&) = 0;
  virtual Status Release(const FrameRef&) = 0;

  // Residency is store state rather than frame identity, so it is queried, never carried on
  // the handle: a FrameRef copied before a spill would otherwise lie about where its bytes are.
  virtual Result<Residency> ResidencyOf(const FrameRef& frame) = 0;

  virtual Status Demote(const FrameRef&, Residency target) = 0;
  virtual Status Forget(const FrameRef&) = 0;
  virtual Result<uint64_t> ContentHash(const FrameRef&) = 0;
};

}  // namespace sphanorama
