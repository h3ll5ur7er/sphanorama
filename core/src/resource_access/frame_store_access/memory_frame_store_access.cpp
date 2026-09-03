#include "resource_access/frame_store_access/memory_frame_store_access.h"

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

MemoryFrameStoreAccess::MemoryFrameStoreAccess(int64_t heapCeilingBytes)
    : ceiling_(heapCeilingBytes) {}

MemoryFrameStoreAccess::Entry* MemoryFrameStoreAccess::Find(const FrameRef& frame) {
  const auto it = entries_.find(frame.id.value);
  return it == entries_.end() ? nullptr : &it->second;
}

void MemoryFrameStoreAccess::Reclassify(Entry& entry, Residency target) {
  if (IsHeapTier(entry.residency) == IsHeapTier(target)) {
    entry.residency = target;
    return;
  }
  const auto size = static_cast<int64_t>(entry.bytes.size());
  if (IsHeapTier(target)) {
    spilled_ -= size;
    heap_used_ += size;
  } else {
    heap_used_ -= size;
    spilled_ += size;
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

  FrameRef frame;
  frame.id = FrameId{next_id_};
  frame.buffer = BufferId{next_id_};
  frame.format = format;
  frame.width = width;
  frame.height = height;
  frame.stride = width * BytesPerPixel(format);
  ++next_id_;

  Entry entry;
  entry.bytes.assign(static_cast<size_t>(size), 0);
  entry.residency = Residency::HeapEncoded;
  entries_.emplace(frame.id.value, std::move(entry));
  heap_used_ += size;
  return Ok(frame);
}

Result<std::span<uint8_t>> MemoryFrameStoreAccess::Pin(const FrameRef& frame) {
  Entry* entry = Find(frame);
  if (entry == nullptr) {
    return Err<std::span<uint8_t>>(StatusCode::NotFound, kComponent, "no such frame");
  }
  if (entry->residency == Residency::Spilled) {
    // Spilling gave the budget back and something else may have taken it since. Promoting without
    // re-checking the ceiling is how a store that exists to model memory pressure quietly stops
    // modelling it: the manager tests keep passing while the device is over its budget.
    const auto size = static_cast<int64_t>(entry->bytes.size());
    if (heap_used_ + size > ceiling_) {
      return Err<std::span<uint8_t>>(StatusCode::FrameStoreExhausted, kComponent,
                                     "faulting this frame in would exceed the heap ceiling");
    }
  }
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
  if (entry->pins > 0) {
    return Fail(StatusCode::FailedPrecondition, kComponent, "cannot demote a pinned frame");
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
  const auto size = static_cast<int64_t>(entry->bytes.size());
  if (IsHeapTier(entry->residency)) {
    heap_used_ -= size;
  } else {
    spilled_ -= size;
  }
  entries_.erase(frame.id.value);
  return Status::Ok();
}

Result<uint64_t> MemoryFrameStoreAccess::ContentHash(const FrameRef& frame) {
  Entry* entry = Find(frame);
  if (entry == nullptr) return Err<uint64_t>(StatusCode::NotFound, kComponent, "no such frame");
  return Ok(HashBytes(entry->bytes));
}

}  // namespace sphanorama
