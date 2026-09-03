#include "support/fake_frame_store_access.h"

namespace sphanorama {
namespace {

constexpr const char* kComponent = "FakeFrameStoreAccess";

// FNV-1a. The real store will use something faster; what the contract requires is only that the
// value depends on the bytes and nothing else.
uint64_t HashBytes(const std::vector<uint8_t>& bytes) {
  uint64_t hash = 1469598103934665603ull;
  for (const uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}

}  // namespace

FakeFrameStoreAccess::FakeFrameStoreAccess(int64_t heapCeilingBytes) : ceiling_(heapCeilingBytes) {}

FakeFrameStoreAccess::Entry* FakeFrameStoreAccess::Find(const FrameRef& frame) {
  const auto it = entries_.find(frame.id.value);
  return it == entries_.end() ? nullptr : &it->second;
}

Result<FrameStoreBudget> FakeFrameStoreAccess::Budget() {
  FrameStoreBudget budget;
  budget.heapCeilingBytes = ceiling_;
  for (const auto& [id, entry] : entries_) {
    const auto size = static_cast<int64_t>(entry.bytes.size());
    if (entry.residency == Residency::Spilled) {
      budget.spilledBytes += size;
    } else {
      budget.heapUsedBytes += size;
    }
  }
  return Ok(budget);
}

Result<FrameRef> FakeFrameStoreAccess::Allocate(int32_t width, int32_t height,
                                                PixelFormat format) {
  const int64_t size = FrameByteSize(width, height, format);
  if (size <= 0) {
    return Err<FrameRef>(StatusCode::InvalidArgument, kComponent,
                         "frame has no representable size");
  }
  if (Budget().value.heapUsedBytes + size > ceiling_) {
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
  return Ok(frame);
}

Result<std::span<uint8_t>> FakeFrameStoreAccess::Pin(const FrameRef& frame) {
  Entry* entry = Find(frame);
  if (entry == nullptr) {
    return Err<std::span<uint8_t>>(StatusCode::NotFound, kComponent, "no such frame");
  }
  if (entry->residency == Residency::Spilled) {
    // Spilling gave the budget back and something else may have taken it. Promoting without
    // re-checking is how a store that exists to model memory pressure stops modelling it.
    const auto size = static_cast<int64_t>(entry->bytes.size());
    if (Budget().value.heapUsedBytes + size > ceiling_) {
      return Err<std::span<uint8_t>>(StatusCode::FrameStoreExhausted, kComponent,
                                     "faulting this frame in would exceed the heap ceiling");
    }
  }
  ++pin_count_;
  ++entry->pins;
  entry->residency = Residency::HeapPinned;   // faulting in from spill is a no-op in memory
  return Ok(std::span<uint8_t>(entry->bytes));
}

Status FakeFrameStoreAccess::Release(const FrameRef& frame) {
  Entry* entry = Find(frame);
  if (entry == nullptr) return Fail(StatusCode::NotFound, kComponent, "no such frame");
  if (entry->pins == 0) {
    return Fail(StatusCode::FailedPrecondition, kComponent, "frame is not pinned");
  }
  if (--entry->pins == 0) entry->residency = Residency::HeapEncoded;
  return Status::Ok();
}

Result<Residency> FakeFrameStoreAccess::ResidencyOf(const FrameRef& frame) {
  Entry* entry = Find(frame);
  if (entry == nullptr) return Err<Residency>(StatusCode::NotFound, kComponent, "no such frame");
  return Ok(entry->residency);
}

Status FakeFrameStoreAccess::Demote(const FrameRef& frame, Residency target) {
  Entry* entry = Find(frame);
  if (entry == nullptr) return Fail(StatusCode::NotFound, kComponent, "no such frame");
  if (entry->pins > 0) {
    return Fail(StatusCode::FailedPrecondition, kComponent, "cannot demote a pinned frame");
  }
  if (target == Residency::Spilled) ++spill_count_;
  entry->residency = target;
  return Status::Ok();
}

Status FakeFrameStoreAccess::Forget(const FrameRef& frame) {
  Entry* entry = Find(frame);
  if (entry == nullptr) return Fail(StatusCode::NotFound, kComponent, "no such frame");
  if (entry->pins > 0) {
    // Pin promises its span stays valid until Release. Erasing the entry under that promise is a
    // use-after-free for whoever still holds the span, and the manager's rollback paths call
    // Forget on frames they may not have released.
    return Fail(StatusCode::FailedPrecondition, kComponent, "frame is still pinned");
  }
  entries_.erase(frame.id.value);
  return Status::Ok();
}

Result<uint64_t> FakeFrameStoreAccess::ContentHash(const FrameRef& frame) {
  Entry* entry = Find(frame);
  if (entry == nullptr) return Err<uint64_t>(StatusCode::NotFound, kComponent, "no such frame");
  return Ok(HashBytes(entry->bytes));
}

}  // namespace sphanorama
