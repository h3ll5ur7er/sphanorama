// The arena exists so per-stage peak memory is bounded and measurable: an OOM on a phone is a
// fatal page crash, not something we could catch. Exhaustion therefore has to be a value, not a
// throw, and these tests pin that behaviour.
#include <gtest/gtest.h>

#include <cstdint>

#include "utilities/arena.h"

namespace sphanorama {
namespace {

TEST(BumpArena, ReportsTheCapacityItWasGiven) {
  BumpArena arena(1024);
  EXPECT_EQ(arena.Capacity(), 1024);
  EXPECT_EQ(arena.Mark(), 0);
}

TEST(BumpArena, HandsOutTheRequestedNumberOfBytes) {
  BumpArena arena(1024);
  auto block = arena.Take(64, 1);
  EXPECT_EQ(block.size(), 64u);
}

TEST(BumpArena, SuccessiveBlocksDoNotOverlap) {
  BumpArena arena(1024);
  auto a = arena.Take(64, 1);
  auto b = arena.Take(64, 1);
  ASSERT_EQ(a.size(), 64u);
  ASSERT_EQ(b.size(), 64u);
  EXPECT_GE(b.data(), a.data() + a.size());
}

TEST(BumpArena, RespectsTheRequestedAlignment) {
  BumpArena arena(1024);
  arena.Take(1, 1);                       // knock the offset off alignment
  auto block = arena.Take(64, 64);
  ASSERT_EQ(block.size(), 64u);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(block.data()) % 64u, 0u);
}

TEST(BumpArena, ExhaustionReturnsAnEmptySpanRatherThanFailing) {
  BumpArena arena(128);
  auto ok = arena.Take(100, 1);
  ASSERT_EQ(ok.size(), 100u);
  auto too_big = arena.Take(100, 1);
  EXPECT_TRUE(too_big.empty());
}

TEST(BumpArena, StaysUsableAfterARefusedAllocation) {
  // A refused request must not consume budget, or one oversized ask would poison the stage.
  BumpArena arena(128);
  arena.Take(100, 1);
  const int64_t mark = arena.Mark();
  EXPECT_TRUE(arena.Take(100, 1).empty());
  EXPECT_EQ(arena.Mark(), mark);
  EXPECT_EQ(arena.Take(16, 1).size(), 16u);
}

TEST(BumpArena, ResetToRewindsAndReusesTheSameMemory) {
  BumpArena arena(1024);
  const int64_t mark = arena.Mark();
  auto first = arena.Take(64, 8);
  arena.ResetTo(mark);
  EXPECT_EQ(arena.Mark(), mark);
  auto second = arena.Take(64, 8);
  EXPECT_EQ(second.data(), first.data());
}

TEST(BumpArena, IgnoresAMarkThatWouldMoveTheOffsetForward) {
  BumpArena arena(1024);
  arena.Take(64, 1);
  const int64_t mark = arena.Mark();
  arena.ResetTo(mark + 512);   // nonsense: would hand out memory never allocated
  EXPECT_EQ(arena.Mark(), mark);
  arena.ResetTo(-1);
  EXPECT_EQ(arena.Mark(), mark);
}

TEST(BumpArena, RejectsNonsenseRequests) {
  BumpArena arena(1024);
  EXPECT_TRUE(arena.Take(0, 1).empty());
  EXPECT_TRUE(arena.Take(-8, 1).empty());
  EXPECT_TRUE(arena.Take(8, 0).empty());
  EXPECT_TRUE(arena.Take(8, 3).empty());   // alignment must be a power of two
  EXPECT_EQ(arena.Mark(), 0);
}

}  // namespace
}  // namespace sphanorama
