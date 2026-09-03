#include "utilities/arena.h"

#include <cstddef>

namespace sphanorama {
namespace {

bool IsPowerOfTwo(int64_t v) { return v > 0 && (v & (v - 1)) == 0; }

}  // namespace

BumpArena::BumpArena(int64_t capacityBytes)
    : storage_(capacityBytes > 0 ? static_cast<size_t>(capacityBytes) : 0u) {}

std::span<uint8_t> BumpArena::Take(int64_t bytes, int64_t alignment) {
  if (bytes <= 0 || !IsPowerOfTwo(alignment)) return {};

  // Align the absolute address, not the offset: the backing buffer's own address is not
  // guaranteed to satisfy an alignment stricter than max_align_t.
  const auto base = reinterpret_cast<uintptr_t>(storage_.data());
  const uintptr_t mask = static_cast<uintptr_t>(alignment) - 1u;
  const uintptr_t aligned = (base + static_cast<uintptr_t>(offset_) + mask) & ~mask;
  const int64_t start = static_cast<int64_t>(aligned - base);

  if (start > Capacity() || bytes > Capacity() - start) return {};

  offset_ = start + bytes;
  return {storage_.data() + start, static_cast<size_t>(bytes)};
}

int64_t BumpArena::Mark() const { return offset_; }

void BumpArena::ResetTo(int64_t mark) {
  // Only ever rewind. A mark beyond the current offset would hand out memory that was never
  // allocated, which is a caller bug we would rather ignore than propagate.
  if (mark >= 0 && mark <= offset_) offset_ = mark;
}

int64_t BumpArena::Capacity() const { return static_cast<int64_t>(storage_.size()); }

}  // namespace sphanorama
