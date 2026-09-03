#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

#include "sphanorama/resource_access/frame_store_access.h"

namespace sphanorama {

// V11 for everything that is not a phone: the bench, the native suite, and any host with room to
// spare. Frames live in one allocation each, under a ceiling the caller states.
//
// **Its spill tier is still RAM.** Demoting moves a frame out of the heap budget and into the
// spilled one, and Pin faults it back — the arithmetic and the fault-in path are real, and every
// caller that has to cope with a frame not being resident is exercised. What does not happen is
// the part that matters on a phone: the bytes are never handed to anything outside the process,
// so spilling here frees budget without freeing memory. That is why this is the native store and
// why the browser needs its own, over OPFS.
//
// Modelling the ceiling is the point. Every interesting bug in a frame store is about running out
// of room, and a store with unlimited memory would let manager tests pass while a phone dies.
class MemoryFrameStoreAccess final : public IFrameStoreAccess {
 public:
  // No default. How much memory there is to spend is a property of the host, so the composition
  // root states it — a store that guessed would be the "assumed, not measured" that
  // FrameStoreBudget::heapCeilingBytes exists to rule out.
  explicit MemoryFrameStoreAccess(int64_t heapCeilingBytes);

  Result<FrameStoreBudget> Budget() override;
  Result<FrameRef> Allocate(int32_t width, int32_t height, PixelFormat format) override;
  Result<std::span<uint8_t>> Pin(const FrameRef& frame) override;
  Status Release(const FrameRef& frame) override;
  Result<Residency> ResidencyOf(const FrameRef& frame) override;
  Status Demote(const FrameRef& frame, Residency target) override;
  Status Forget(const FrameRef& frame) override;
  Result<uint64_t> ContentHash(const FrameRef& frame) override;

 private:
  struct Entry {
    std::vector<uint8_t> bytes;
    Residency residency = Residency::HeapEncoded;
    int pins = 0;
  };

  Entry* Find(const FrameRef& frame);
  // Moves an entry's bytes between the two budget tiers. Every residency change goes through it,
  // so the totals cannot drift from the entries they are meant to describe.
  void Reclassify(Entry& entry, Residency target);

  std::map<uint64_t, Entry> entries_;
  const int64_t ceiling_;
  // Kept as running totals rather than summed on demand: Budget is read inside Allocate and Pin,
  // both of which run per captured frame, and a sphere holds hundreds. Walking every entry to
  // answer "how much is left" would make the store quadratic in the thing it exists to bound.
  int64_t heap_used_ = 0;
  int64_t spilled_ = 0;
  uint64_t next_id_ = 1;
};

struct MemoryFrameStoreAccessFactory {
  static std::unique_ptr<IFrameStoreAccess> Create() {
    // Small on purpose: the suite's exhaustion cases have to be reachable in a test, and a
    // ceiling nothing can hit is a ceiling nothing tests.
    return std::make_unique<MemoryFrameStoreAccess>(1 << 20);
  }
};

}  // namespace sphanorama
