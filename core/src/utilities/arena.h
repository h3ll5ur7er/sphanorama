#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "sphanorama/utilities/arena.h"

namespace sphanorama {

// A fixed-capacity bump allocator. Allocation is a pointer add; there is no per-block free, only
// ResetTo, because stage-scoped scratch is the only thing this is for.
//
// Exhaustion returns an empty span and leaves the arena untouched. That is deliberate: a refused
// request must not consume budget, or one oversized ask would poison the rest of the stage.
class BumpArena final : public IArena {
 public:
  explicit BumpArena(int64_t capacityBytes);

  std::span<uint8_t> Take(int64_t bytes, int64_t alignment) override;
  int64_t Mark() const override;
  void ResetTo(int64_t mark) override;
  int64_t Capacity() const override;

 private:
  std::vector<uint8_t> storage_;
  int64_t offset_ = 0;
};

}  // namespace sphanorama
