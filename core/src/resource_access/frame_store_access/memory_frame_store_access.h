#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "resource_access/frame_store_access/spill_sink.h"
#include "sphanorama/resource_access/frame_store_access.h"

namespace sphanorama {

// V11: frames live in one heap allocation each, under a ceiling the caller states, and a spilled
// frame's bytes go wherever the injected ISpillSink puts them.
//
// One store rather than two, because the tiering, the ceiling and the fault-in on Pin are the
// same everywhere and only the destination differs — RAM on a desktop, an OPFS sync access handle
// on a phone. Two implementations would have meant two copies of the arithmetic that decides
// whether a sphere fits, held equal by a contract suite instead of by construction.
//
// **With no sink there is no spill tier at all**, and demoting to one is refused. It used to
// relabel — the frame moved between the two budget totals and kept its vector — and that is a
// store freeing budget without freeing memory, which is the one lie a component that exists to
// model memory pressure cannot afford to tell. Whether a frame can leave the heap is decided by
// whether there is somewhere for it to go, on every platform, and nothing else.
//
// Modelling the ceiling is the point. Every interesting bug in a frame store is about running out
// of room, and a store with unlimited memory would let manager tests pass while a phone dies.
class MemoryFrameStoreAccess final : public IFrameStoreAccess {
 public:
  // No default ceiling. How much memory there is to spend is a property of the host, so the
  // composition root states it. The sink is optional and non-owning: a host with nowhere to put a
  // spilled frame passes nothing, and its store refuses to spill rather than pretending to.
  explicit MemoryFrameStoreAccess(int64_t heapCeilingBytes, ISpillSink* spill = nullptr);

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
    // Empty while the frame is spilled to a sink, which is why `size` is carried separately: the
    // budget arithmetic and the fault-in both need to know how big the frame is at a moment when
    // the bytes are not here to ask.
    std::vector<uint8_t> bytes;
    int64_t size = 0;
    // Taken as the bytes left for the sink and read back by ContentHash while they are gone. It
    // cannot go stale: a spilled frame cannot be pinned, so nothing can write to it.
    uint64_t spilledHash = 0;
    // Whether the sink is holding a copy, which is not the same question as whether the frame is
    // spilled. A fault-in reads the bytes back and leaves the copy where it is — taking it away
    // would make a successful Pin depend on a cleanup that has nothing to do with it — so a frame
    // can be resident and still have something down there with its name on it.
    bool inSink = false;
    Residency residency = Residency::HeapEncoded;
    int pins = 0;
  };

  Entry* Find(const FrameRef& frame);
  // Brings a spilled frame's bytes back, ceiling first, and leaves the residency to the caller.
  // Pin needs it and so does a demotion that names a heap tier, and one copy is what keeps the
  // budget and the bytes from disagreeing about where the frame is.
  Status FaultIn(Entry& entry, uint64_t id);
  // Moves an entry's bytes between the two budget tiers. Every residency change goes through it,
  // so the totals cannot drift from the entries they are meant to describe.
  void Reclassify(Entry& entry, Residency target);

  // Why the last spill was refused, empty when the last one was not. A refused spill and a
  // capture genuinely too large for the device both end at an allocation that does not fit, and
  // they are opposite problems — a full disk against a sphere the phone cannot hold. Without this
  // the refusal names the ceiling and sends whoever reads it looking in the wrong place.
  //
  // The last one rather than a count: a sink that started working again should stop being blamed,
  // and what the reader needs is what is wrong now.
  std::string spill_refusal_;

  std::map<uint64_t, Entry> entries_;
  ISpillSink* spill_;
  const int64_t ceiling_;
  // Kept as running totals rather than summed on demand: Budget is read inside Allocate and Pin,
  // both of which run per captured frame, and a sphere holds hundreds. Walking every entry to
  // answer "how much is left" would make the store quadratic in the thing it exists to bound.
  int64_t heap_used_ = 0;
  int64_t spilled_ = 0;
  uint64_t next_id_ = 1;
};

}  // namespace sphanorama
