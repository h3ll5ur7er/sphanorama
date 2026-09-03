#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "sphanorama/resource_access/frame_store_access.h"
#include "utilities/pixel_format.h"

namespace sphanorama {

// An in-memory frame store that models the real one's *behaviour*, not its storage: it has a
// heap ceiling, it refuses allocations it cannot fit, and "spilled" bytes move out of the heap
// budget into a separate buffer that Pin faults back in.
//
// Modelling the ceiling matters more than modelling OPFS. Every interesting bug in the real store
// is about running out of room, and a fake with unlimited memory would let manager tests pass
// while the phone crashes.
class FakeFrameStoreAccess final : public IFrameStoreAccess {
 public:
  explicit FakeFrameStoreAccess(int64_t heapCeilingBytes = 1 << 20);

  Result<FrameStoreBudget> Budget() override;
  Result<FrameRef> Allocate(int32_t width, int32_t height, PixelFormat) override;
  Result<std::span<uint8_t>> Pin(const FrameRef&) override;
  Status Release(const FrameRef&) override;
  Result<Residency> ResidencyOf(const FrameRef&) override;
  Status Demote(const FrameRef&, Residency target) override;
  Status Forget(const FrameRef&) override;
  Result<uint64_t> ContentHash(const FrameRef&) override;

  // Test affordances, not part of the contract.
  int PinCount() const { return pin_count_; }
  int SpillCount() const { return spill_count_; }

 private:
  struct Entry {
    std::vector<uint8_t> bytes;
    Residency residency = Residency::HeapEncoded;
    int pins = 0;
  };

  Entry* Find(const FrameRef&);

  std::map<uint64_t, Entry> entries_;
  int64_t ceiling_;
  uint64_t next_id_ = 1;
  int pin_count_ = 0;
  int spill_count_ = 0;
};

struct FakeFrameStoreAccessFactory {
  static std::unique_ptr<IFrameStoreAccess> Create() {
    return std::make_unique<FakeFrameStoreAccess>();
  }
};

}  // namespace sphanorama
