#include "resource_access/frame_store_access/memory_frame_store_access.h"

#include <limits>
#include <utility>

#include "utilities/pixel_format.h"

namespace sphanorama {
namespace {

constexpr const char* kComponent = "MemoryFrameStoreAccess";

// FNV-1a. What the contract asks of a content hash is only that it depends on the bytes and
// nothing else — not that it is fast, and not that it is the same function the browser store
// picks. Two stores that disagreed on the value would still both satisfy it; two stores that
// disagreed on *whether equal bytes hash equally* would not.
uint64_t HashBytes(const std::vector<uint8_t>& bytes) {
  uint64_t hash = 1469598103934665603ull;
  for (const uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}

bool IsHeapTier(Residency residency) { return residency != Residency::Spilled; }

}  // namespace

MemoryFrameStoreAccess::MemoryFrameStoreAccess(int64_t heapCeilingBytes, ISpillSink* spill)
    : spill_(spill), ceiling_(heapCeilingBytes) {}

MemoryFrameStoreAccess::Entry* MemoryFrameStoreAccess::Find(const FrameRef& frame) {
  const auto it = entries_.find(frame.id.value);
  return it == entries_.end() ? nullptr : &it->second;
}

void MemoryFrameStoreAccess::Reclassify(Entry& entry, Residency target) {
  if (IsHeapTier(entry.residency) == IsHeapTier(target)) {
    entry.residency = target;
    return;
  }
  if (IsHeapTier(target)) {
    spilled_ -= entry.size;
    heap_used_ += entry.size;
  } else {
    heap_used_ -= entry.size;
    spilled_ += entry.size;
  }
  entry.residency = target;
}

Result<FrameStoreBudget> MemoryFrameStoreAccess::Budget() {
  FrameStoreBudget budget;
  budget.heapCeilingBytes = ceiling_;
  budget.heapUsedBytes = heap_used_;
  budget.spilledBytes = spilled_;
  return Ok(budget);
}

Result<FrameRef> MemoryFrameStoreAccess::Allocate(int32_t width, int32_t height,
                                                  PixelFormat format) {
  const int64_t size = FrameByteSize(width, height, format);
  if (size <= 0) {
    return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                         "frame has no representable size");
  }
  if (heap_used_ + size > ceiling_) {
    // Refused, never attempted: an allocation that overruns on a phone is not an exception, it is
    // a page the operating system kills. The caller is told before that happens.
    return Err<FrameRef>(StatusCode::FrameStoreExhausted, kComponent,
                         "allocation would exceed the heap ceiling");
  }

  // In int64 and range-checked, because the obvious form is int32 * int32: FrameByteSize is
  // careful to guard its own arithmetic and this line threw that away, so a width near INT32_MAX
  // passed the size check and then overflowed here. Signed overflow is undefined behaviour, not
  // a wrong number the caller could sanity-check.
  //
  // A planar format's stride is its luma row, and BytesPerPixel reports 0 for those — it is
  // documented as being for interleaved stride arithmetic. Taking it literally would hand back a
  // stride of zero for a frame that allocated perfectly well.
  const int32_t perPixel = BytesPerPixel(format);
  const int64_t stride = perPixel > 0 ? static_cast<int64_t>(width) * perPixel
                                      : static_cast<int64_t>(width);
  if (stride > std::numeric_limits<int32_t>::max()) {
    return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                         "a row of this frame does not fit a representable stride");
  }

  FrameRef frame;
  frame.id = FrameId{next_id_};
  frame.buffer = BufferId{next_id_};
  frame.format = format;
  frame.width = width;
  frame.height = height;
  frame.stride = static_cast<int32_t>(stride);
  ++next_id_;

  Entry entry;
  entry.size = size;
  entry.bytes.assign(static_cast<size_t>(size), 0);
  entry.residency = Residency::HeapEncoded;
  entries_.emplace(frame.id.value, std::move(entry));
  heap_used_ += size;
  return Ok(frame);
}

Status MemoryFrameStoreAccess::FaultIn(Entry& entry, uint64_t id) {
  if (entry.residency != Residency::Spilled) return Status::Ok();

  // Spilling gave the budget back and something else may have taken it since. Reading without
  // re-checking the ceiling is how a store that exists to model memory pressure quietly stops
  // modelling it: the manager tests keep passing while the device is over its budget. Checked
  // before the read, not after, because the bytes are in hand by then and the damage is done.
  if (heap_used_ + entry.size > ceiling_) {
    return Fail(StatusCode::FrameStoreExhausted, kComponent,
                "faulting this frame in would exceed the heap ceiling");
  }
  if (spill_ == nullptr) return Status::Ok();   // never left; the vector is still here

  // Read into a buffer of its own and moved in only once it is whole, rather than filled in
  // place. A read that failed halfway would otherwise leave the entry holding a frame-sized
  // allocation full of nothing while still classified as spilled — memory the store does not
  // charge itself for, on a device that spilled precisely because it had none. Rolling that back
  // by hand is a line nothing can observe and everything depends on; not creating the state is
  // better than remembering to undo it.
  std::vector<uint8_t> bytes(static_cast<size_t>(entry.size));
  if (auto read = spill_->Read(id, std::span<uint8_t>(bytes)); !read.ok()) return read;
  entry.bytes = std::move(bytes);
  return Status::Ok();
}

Result<std::span<uint8_t>> MemoryFrameStoreAccess::Pin(const FrameRef& frame) {
  Entry* entry = Find(frame);
  if (entry == nullptr) {
    return Err<std::span<uint8_t>>(StatusCode::NotFound, kComponent, "no such frame");
  }
  if (auto faulted = FaultIn(*entry, frame.id.value); !faulted.ok()) return faulted;
  Reclassify(*entry, Residency::HeapPinned);
  ++entry->pins;
  return Ok(std::span<uint8_t>(entry->bytes));
}

Status MemoryFrameStoreAccess::Release(const FrameRef& frame) {
  Entry* entry = Find(frame);
  if (entry == nullptr) return Fail(StatusCode::NotFound, kComponent, "no such frame");
  if (entry->pins == 0) {
    return Fail(StatusCode::FailedPrecondition, kComponent, "frame is not pinned");
  }
  // Nested pins are counted rather than collapsed: two engines may hold the same frame, and the
  // first one to finish must not invalidate the other's span.
  if (--entry->pins == 0) Reclassify(*entry, Residency::HeapEncoded);
  return Status::Ok();
}

Result<Residency> MemoryFrameStoreAccess::ResidencyOf(const FrameRef& frame) {
  Entry* entry = Find(frame);
  if (entry == nullptr) return Err<Residency>(StatusCode::NotFound, kComponent, "no such frame");
  return Ok(entry->residency);
}

Status MemoryFrameStoreAccess::Demote(const FrameRef& frame, Residency target) {
  Entry* entry = Find(frame);
  if (entry == nullptr) return Fail(StatusCode::NotFound, kComponent, "no such frame");
  // Not every residency is something a demotion can produce, and accepting them all made
  // ResidencyOf report states that were not true. Pinned is the clearest: it meant the store
  // claimed a mapping nothing had established, and the Release that followed failed.
  if (target == Residency::HeapPinned) {
    return Fail(StatusCode::InvalidArgument, kComponent,
                "pinning is what Pin is for; a demotion cannot produce it");
  }
  if (target == Residency::GpuTexture) {
    return Fail(StatusCode::Unsupported, kComponent, "this store has no GPU tier");
  }
  if (entry->pins > 0) {
    return Fail(StatusCode::FailedPrecondition, kComponent, "cannot demote a pinned frame");
  }

  if (target == Residency::Spilled && entry->residency != Residency::Spilled &&
      spill_ != nullptr) {
    // The write comes first and the heap is freed only once it succeeds. A sink out of quota is
    // the ordinary case on a phone, and a demotion that dropped the bytes and then reported
    // failure would lose a captured cell to a full disk.
    if (auto written = spill_->Write(frame.id.value, std::span<const uint8_t>(entry->bytes));
        !written.ok()) {
      return written;
    }
    entry->spilledHash = HashBytes(entry->bytes);
    std::vector<uint8_t>().swap(entry->bytes);
  } else if (target != Residency::Spilled && entry->residency == Residency::Spilled) {
    // Demote is named for the direction it usually goes, and the contract lets a caller name any
    // producible tier — including a heap one. Reclassifying without the bytes would leave a frame
    // that reports itself resident and pins to nothing.
    if (auto faulted = FaultIn(*entry, frame.id.value); !faulted.ok()) return faulted;
  }

  Reclassify(*entry, target);
  return Status::Ok();
}

Status MemoryFrameStoreAccess::Forget(const FrameRef& frame) {
  Entry* entry = Find(frame);
  if (entry == nullptr) return Fail(StatusCode::NotFound, kComponent, "no such frame");
  if (entry->pins > 0) {
    // Pin promises its span stays valid until Release. Erasing the entry under that promise is a
    // use-after-free for whoever still holds the span, and the manager's rollback paths call
    // Forget on frames they may not have released.
    return Fail(StatusCode::FailedPrecondition, kComponent, "frame is still pinned");
  }
  // Taken off whichever total was holding it. Going through Reclassify first would work and
  // would read as a spill that is really a deletion.
  if (entry->residency == Residency::Spilled && spill_ != nullptr) {
    // Nothing else ever will. The store is the only thing that knows this frame is down there,
    // and a sphere's worth of abandoned bursts would fill the device's quota with bytes no code
    // path can name. A refusal is reported rather than swallowed, and the entry stays, so the
    // budget keeps accounting for what the sink still holds.
    if (auto dropped = spill_->Drop(frame.id.value); !dropped.ok()) return dropped;
  }
  if (IsHeapTier(entry->residency)) {
    heap_used_ -= entry->size;
  } else {
    spilled_ -= entry->size;
  }
  entries_.erase(frame.id.value);
  return Status::Ok();
}

Result<uint64_t> MemoryFrameStoreAccess::ContentHash(const FrameRef& frame) {
  Entry* entry = Find(frame);
  if (entry == nullptr) return Err<uint64_t>(StatusCode::NotFound, kComponent, "no such frame");
  // A frame in the sink still has a content hash, and the build graph's fingerprints depend on
  // it: hashing the empty vector left behind would make every spilled frame identical to every
  // other, so an incremental rebuild would reuse one cell's work for another's.
  if (entry->residency == Residency::Spilled && spill_ != nullptr) return Ok(entry->spilledHash);
  return Ok(HashBytes(entry->bytes));
}

}  // namespace sphanorama
