#include "utilities/clock.h"

#include <chrono>

namespace sphanorama {

int64_t SystemClock::MonotonicNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int64_t SystemClock::WallMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

ManualClock::ManualClock(int64_t wallStartMs) : wall_start_ms_(wallStartMs) {}

int64_t ManualClock::MonotonicNs() { return elapsed_ns_; }

int64_t ManualClock::WallMs() { return wall_start_ms_ + elapsed_ns_ / 1'000'000; }

void ManualClock::AdvanceNs(int64_t ns) {
  if (ns > 0) elapsed_ns_ += ns;
}

void ManualClock::AdvanceMs(int64_t ms) { AdvanceNs(ms * 1'000'000); }

}  // namespace sphanorama
