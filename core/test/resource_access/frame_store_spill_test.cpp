// What a spill does when there is somewhere to spill *to*.
//
// The contract suite next door covers the store's behaviour with no sink, where "spilled" is a
// budget classification and the bytes never move. That is honest on a desktop and a lie on a
// phone, which is the whole reason this seam exists — so these tests are about the bytes actually
// leaving and coming back, and about what the store does when the place they went says no.
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "support/fake_spill_sink.h"

namespace sphanorama {
namespace {

constexpr int32_t kWidth = 8;
constexpr int32_t kHeight = 8;
constexpr int64_t kFrameBytes = kWidth * kHeight * 4;

class FrameStoreSpill : public ::testing::Test {
 protected:
  FakeSpillSink sink;
  MemoryFrameStoreAccess store{1 << 20, &sink};

  FrameRef Allocate() {
    auto allocated = store.Allocate(kWidth, kHeight, PixelFormat::RGBA8);
    EXPECT_TRUE(allocated.ok()) << allocated.status.detail;
    return allocated.value;
  }

  void Fill(const FrameRef& frame, uint8_t value) {
    auto pinned = store.Pin(frame);
    ASSERT_TRUE(pinned.ok()) << pinned.status.detail;
    std::fill(pinned.value.begin(), pinned.value.end(), value);
    ASSERT_TRUE(store.Release(frame).ok());
  }

  int64_t HeapUsed() {
    auto budget = store.Budget();
    EXPECT_TRUE(budget.ok());
    return budget.value.heapUsedBytes;
  }
};

TEST_F(FrameStoreSpill, SpillingHandsTheBytesToTheSinkRatherThanRelabellingThem) {
  // Without a sink the store moves a number from one total to the other and keeps the vector,
  // which frees budget without freeing memory. On a phone that is the difference between a
  // sphere that fits and a tab the operating system kills.
  const FrameRef frame = Allocate();
  Fill(frame, 0xA7);

  ASSERT_TRUE(store.Demote(frame, Residency::Spilled).ok());
  EXPECT_EQ(sink.Writes(), 1);
  ASSERT_TRUE(sink.Holds(frame.id.value));
  EXPECT_EQ(sink.Held(frame.id.value).size(), static_cast<size_t>(kFrameBytes));
  EXPECT_EQ(sink.Held(frame.id.value).front(), 0xA7);
  EXPECT_EQ(HeapUsed(), 0);
}

TEST_F(FrameStoreSpill, FaultingInReadsTheBytesBackByteForByte) {
  // The whole point of a spill tier: what comes back is what went out. A store that faulted in
  // zeroes would produce a stitch nobody could trace back to here, because the frames either
  // side of it would be perfect.
  const FrameRef frame = Allocate();
  Fill(frame, 0x3C);
  ASSERT_TRUE(store.Demote(frame, Residency::Spilled).ok());

  auto pinned = store.Pin(frame);
  ASSERT_TRUE(pinned.ok()) << pinned.status.detail;
  EXPECT_EQ(pinned.value.size(), static_cast<size_t>(kFrameBytes));
  EXPECT_TRUE(std::all_of(pinned.value.begin(), pinned.value.end(),
                          [](uint8_t b) { return b == 0x3C; }));
  EXPECT_TRUE(store.Release(frame).ok());
  EXPECT_EQ(HeapUsed(), kFrameBytes);
}

TEST_F(FrameStoreSpill, ASinkThatRefusesTheWriteLeavesTheFrameWhereItWas) {
  // A phone out of quota is the ordinary case, not the exotic one. The frame has to still be
  // there afterwards: a demotion that reported failure while dropping the bytes would lose a
  // captured cell to a full disk.
  const FrameRef frame = Allocate();
  Fill(frame, 0x5E);
  sink.FailWrites(true);

  EXPECT_EQ(store.Demote(frame, Residency::Spilled).code, StatusCode::FrameStoreExhausted);
  auto residency = store.ResidencyOf(frame);
  ASSERT_TRUE(residency.ok());
  EXPECT_EQ(residency.value, Residency::HeapEncoded) << "the frame was demoted anyway";
  EXPECT_EQ(HeapUsed(), kFrameBytes) << "the budget moved for a spill that never happened";

  auto pinned = store.Pin(frame);
  ASSERT_TRUE(pinned.ok());
  ASSERT_EQ(pinned.value.size(), static_cast<size_t>(kFrameBytes))
      << "the bytes were freed before the sink accepted them";
  EXPECT_EQ(pinned.value.front(), 0x5E);
  EXPECT_TRUE(store.Release(frame).ok());
}

TEST_F(FrameStoreSpill, ASinkThatRefusesTheReadLeavesTheFrameSpilledRatherThanEmpty) {
  // Pin is the only route to bytes, so a failed fault-in must not look like a successful one.
  // Leaving the frame classified as spilled is what makes the next attempt meaningful.
  const FrameRef frame = Allocate();
  Fill(frame, 0x11);
  ASSERT_TRUE(store.Demote(frame, Residency::Spilled).ok());
  sink.FailReads(true);

  EXPECT_FALSE(store.Pin(frame).ok());
  auto residency = store.ResidencyOf(frame);
  ASSERT_TRUE(residency.ok());
  EXPECT_EQ(residency.value, Residency::Spilled);
  EXPECT_EQ(HeapUsed(), 0) << "a failed fault-in charged the heap for bytes it does not hold";

  // And the failure is not terminal: the next read succeeds and the frame is intact.
  sink.FailReads(false);
  auto pinned = store.Pin(frame);
  ASSERT_TRUE(pinned.ok()) << pinned.status.detail;
  EXPECT_EQ(pinned.value.front(), 0x11);
  EXPECT_TRUE(store.Release(frame).ok());
}

TEST_F(FrameStoreSpill, ForgettingASpilledFrameDropsItFromTheSink) {
  // Nothing else ever will. The store is the only thing that knows this frame is down there, and
  // a sphere's worth of abandoned bursts left behind would fill the device's quota silently.
  const FrameRef frame = Allocate();
  ASSERT_TRUE(store.Demote(frame, Residency::Spilled).ok());

  ASSERT_TRUE(store.Forget(frame).ok());
  EXPECT_EQ(sink.Drops(), 1);
  EXPECT_FALSE(sink.Holds(frame.id.value));
}

TEST_F(FrameStoreSpill, AFrameThatCameBackFromTheSinkIsStillDroppedWhenItIsForgotten) {
  // Residency says where the bytes are *now*, and the sink's copy outlives a fault-in: the read
  // does not take it away, and dropping it there would make a successful Pin depend on a cleanup
  // that has nothing to do with it. So the store has to remember that a copy is down there.
  // Without that, every candidate that was spilled, examined and then discarded — which is what
  // selection does to a burst — would leave its bytes in the file until the session ended.
  const FrameRef frame = Allocate();
  ASSERT_TRUE(store.Demote(frame, Residency::Spilled).ok());
  ASSERT_TRUE(store.Pin(frame).ok());
  ASSERT_TRUE(store.Release(frame).ok());
  ASSERT_TRUE(sink.Holds(frame.id.value)) << "the fault-in was not meant to take the copy away";

  ASSERT_TRUE(store.Forget(frame).ok());
  EXPECT_EQ(sink.Drops(), 1);
  EXPECT_FALSE(sink.Holds(frame.id.value));
}

TEST_F(FrameStoreSpill, TheSinkIsNotAskedAboutAFrameThatNeverLeftTheHeap) {
  // Cheap to get wrong and invisible when it is: a Drop per forgotten frame would turn every
  // discarded burst into a round trip to the file system for bytes that were never written.
  const FrameRef frame = Allocate();
  ASSERT_TRUE(store.Forget(frame).ok());
  EXPECT_EQ(sink.Drops(), 0);
}

TEST_F(FrameStoreSpill, AFrameKeepsItsContentHashWhileItsBytesAreInTheSink) {
  // The build graph fingerprints by content hash, so a spilled frame that hashed the empty vector
  // left behind would be identical to every other spilled frame — and an incremental rebuild
  // would reuse one cell's work for another's. That failure appears as a wrong panorama long
  // after the spill that caused it, which is the worst kind this store can produce.
  const FrameRef frame = Allocate();
  Fill(frame, 0x6D);
  auto resident = store.ContentHash(frame);
  ASSERT_TRUE(resident.ok());

  ASSERT_TRUE(store.Demote(frame, Residency::Spilled).ok());
  auto spilled = store.ContentHash(frame);
  ASSERT_TRUE(spilled.ok());
  EXPECT_EQ(spilled.value, resident.value);

  // And it is the same again once the bytes are back, which is what says the cached answer was
  // the real one rather than a plausible constant.
  ASSERT_TRUE(store.Pin(frame).ok());
  ASSERT_TRUE(store.Release(frame).ok());
  auto faulted = store.ContentHash(frame);
  ASSERT_TRUE(faulted.ok());
  EXPECT_EQ(faulted.value, resident.value);
}

TEST_F(FrameStoreSpill, DemotingASpilledFrameBackToTheHeapBringsItsBytesWithIt) {
  // Demote is named for the direction it usually goes, and the contract lets a caller name any
  // producible tier — a heap one included. Reclassifying without reading the bytes back would
  // leave a frame that reports itself resident and pins to nothing, and the pin would then hand
  // an engine an empty span rather than failing.
  const FrameRef frame = Allocate();
  Fill(frame, 0x9B);
  ASSERT_TRUE(store.Demote(frame, Residency::Spilled).ok());

  ASSERT_TRUE(store.Demote(frame, Residency::HeapEncoded).ok());
  EXPECT_EQ(HeapUsed(), kFrameBytes);
  auto pinned = store.Pin(frame);
  ASSERT_TRUE(pinned.ok()) << pinned.status.detail;
  ASSERT_EQ(pinned.value.size(), static_cast<size_t>(kFrameBytes));
  EXPECT_EQ(pinned.value.front(), 0x9B);
  EXPECT_TRUE(store.Release(frame).ok());
}

TEST_F(FrameStoreSpill, FaultingInStillRespectsTheCeiling) {
  // Spilling gave the budget back and something else took it. The sink changes where the bytes
  // are, not whether there is room for them — and a store that skipped the check because the
  // read succeeded would be over its ceiling with the frame already in hand.
  MemoryFrameStoreAccess tight(kFrameBytes * 2, &sink);
  auto spilled = tight.Allocate(kWidth, kHeight, PixelFormat::RGBA8);
  ASSERT_TRUE(spilled.ok());
  ASSERT_TRUE(tight.Demote(spilled.value, Residency::Spilled).ok());

  std::vector<FrameRef> hogs;
  while (true) {
    auto more = tight.Allocate(kWidth, kHeight, PixelFormat::RGBA8);
    if (!more.ok()) break;
    hogs.push_back(more.value);
  }

  EXPECT_EQ(tight.Pin(spilled.value).status.code, StatusCode::FrameStoreExhausted);
  auto residency = tight.ResidencyOf(spilled.value);
  ASSERT_TRUE(residency.ok());
  EXPECT_EQ(residency.value, Residency::Spilled);
}

}  // namespace
}  // namespace sphanorama
