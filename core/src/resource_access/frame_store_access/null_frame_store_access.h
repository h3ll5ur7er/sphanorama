#pragma once

#include "sphanorama/resource_access/frame_store_access.h"

namespace sphanorama {

// Stands in until the tiered store lands. Reports a zero budget, so a caller that asks what it
// can afford is told the truth rather than being handed a number it cannot spend.
class NullFrameStoreAccess final : public IFrameStoreAccess {
 public:
  Result<FrameStoreBudget> Budget() override;
  Result<FrameRef> Allocate(int32_t width, int32_t height, PixelFormat format) override;
  Result<std::span<uint8_t>> Pin(const FrameRef& frame) override;
  Status Release(const FrameRef& frame) override;
  Result<Residency> ResidencyOf(const FrameRef& frame) override;
  Status Demote(const FrameRef& frame, Residency target) override;
  Status Forget(const FrameRef& frame) override;
  Result<uint64_t> ContentHash(const FrameRef& frame) override;
};

}  // namespace sphanorama
