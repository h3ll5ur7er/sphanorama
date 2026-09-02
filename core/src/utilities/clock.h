#pragma once

#include <cstdint>

#include "sphanorama/utilities/clock.h"

namespace sphanorama {

class SystemClock final : public IClock {
 public:
  int64_t MonotonicNs() override;
  int64_t WallMs() override;
};

// Used by tests and by dataset replay in the bench client, where the recorded timestamps rather
// than the wall clock have to drive burst intervals and stability windows.
class ManualClock final : public IClock {
 public:
  explicit ManualClock(int64_t wallStartMs = 0);

  int64_t MonotonicNs() override;
  int64_t WallMs() override;

  void AdvanceNs(int64_t ns);
  void AdvanceMs(int64_t ms);

 private:
  int64_t wall_start_ms_;
  int64_t elapsed_ns_ = 0;
};

}  // namespace sphanorama
