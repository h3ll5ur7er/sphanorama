#include "utilities/clock.h"

#include <limits>

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

void ManualClock::AdvanceMs(int64_t ms) {
  // Checked before multiplying. AdvanceNs guards its input, but the conversion happens on the way
  // in, so a large millisecond value overflows on the way to being rejected — and signed overflow
  // is undefined, not merely a wrong number the guard could then catch.
  constexpr int64_t kLargest = std::numeric_limits<int64_t>::max() / 1'000'000;
  if (ms <= 0 || ms > kLargest) return;
  AdvanceNs(ms * 1'000'000);
}

}  // namespace sphanorama
