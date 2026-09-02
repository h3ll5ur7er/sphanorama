#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// Bump-allocated scratch for per-stage work. Engines allocate here rather than with new/malloc so
// that peak usage per stage is measurable and bounded — which matters because an OOM on a phone
// is a fatal page crash, not an exception we could recover from.
//
// Exhaustion returns an empty span. There is no failure path to throw down.
class IArena {
 public:
  virtual ~IArena() = default;
  virtual std::span<uint8_t> Take(int64_t bytes, int64_t alignment) = 0;
  virtual int64_t Mark() const = 0;
  virtual void ResetTo(int64_t mark) = 0;
  virtual int64_t Capacity() const = 0;
};

}  // namespace sphanorama
