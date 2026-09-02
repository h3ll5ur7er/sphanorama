#pragma once
#include "sphanorama/types.h"

namespace sphanorama {

// Injected rather than called directly so that time-dependent behaviour (burst intervals,
// stability windows, timeouts) is deterministic under test.
class IClock {
 public:
  virtual ~IClock() = default;
  virtual int64_t MonotonicNs() = 0;
  virtual int64_t WallMs() = 0;
};

}  // namespace sphanorama
