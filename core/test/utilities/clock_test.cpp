// Time is injected rather than called directly so that burst intervals, stability windows and
// timeouts are deterministic under test. ManualClock is the reason manager tests can be fast.
#include <gtest/gtest.h>

#include <limits>

#include "utilities/clock.h"

namespace sphanorama {
namespace {

TEST(SystemClock, MonotonicTimeNeverGoesBackwards) {
  SystemClock clock;
  const int64_t a = clock.MonotonicNs();
  const int64_t b = clock.MonotonicNs();
  EXPECT_GE(b, a);
}

TEST(SystemClock, WallTimeIsPlausible) {
  // Sanity, not precision: anything before 2020 means we wired up the wrong clock.
  SystemClock clock;
  EXPECT_GT(clock.WallMs(), 1577836800000LL);
}

TEST(ManualClock, StartsAtZeroAndOnlyMovesWhenTold) {
  ManualClock clock;
  EXPECT_EQ(clock.MonotonicNs(), 0);
  EXPECT_EQ(clock.MonotonicNs(), 0);
  clock.AdvanceMs(5);
  EXPECT_EQ(clock.MonotonicNs(), 5'000'000);
}

TEST(ManualClock, WallAndMonotonicAdvanceTogether) {
  ManualClock clock(1'700'000'000'000LL);
  clock.AdvanceMs(250);
  EXPECT_EQ(clock.WallMs(), 1'700'000'000'250LL);
  EXPECT_EQ(clock.MonotonicNs(), 250'000'000);
}


TEST(ManualClock, AdvanceMsRefusesADurationItCannotRepresent) {
  // AdvanceNs guards its input, but the multiplication happens on the way in — so a large
  // millisecond value overflows int64 *before* anything can reject it. Signed overflow is
  // undefined, so the wrapped result is not simply a wrong time the caller could sanity-check.
  ManualClock clock;
  clock.AdvanceMs(std::numeric_limits<int64_t>::max());
  EXPECT_EQ(clock.MonotonicNs(), 0);

  clock.AdvanceMs(std::numeric_limits<int64_t>::max() / 1'000'000 + 1);
  EXPECT_EQ(clock.MonotonicNs(), 0);
}

TEST(ManualClock, AdvanceMsStillAcceptsTheLargestDurationThatFits) {
  ManualClock clock;
  const int64_t largest = std::numeric_limits<int64_t>::max() / 1'000'000;
  clock.AdvanceMs(largest);
  EXPECT_EQ(clock.MonotonicNs(), largest * 1'000'000);
}

}  // namespace
}  // namespace sphanorama
