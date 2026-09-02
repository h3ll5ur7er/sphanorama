// The frame-store contract, as a suite that runs against every implementation.
//
// Only the fake is listed today. When the OPFS-backed browser store and the native bench store
// arrive they join the type list below, and the property that matters — that all implementations
// agree — is checked by construction rather than by reading two files side by side.
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

#include "sphanorama/resource_access/frame_store_access.h"
#include "support/fake_frame_store_access.h"

namespace sphanorama {
namespace {

template <typename Factory>
class FrameStoreAccessContract : public ::testing::Test {
 protected:
  std::unique_ptr<IFrameStoreAccess> store = Factory::Create();

  FrameRef Allocate(int32_t w = 4, int32_t h = 4) {
    auto r = store->Allocate(w, h, PixelFormat::RGBA8);
    EXPECT_TRUE(r.ok()) << r.status.detail;
    return r.value;
  }

  void Fill(const FrameRef& frame, uint8_t value) {
    auto pinned = store->Pin(frame);
    ASSERT_TRUE(pinned.ok()) << pinned.status.detail;
    std::fill(pinned.value.begin(), pinned.value.end(), value);
    ASSERT_TRUE(store->Release(frame).ok());
  }

  uint8_t FirstByte(const FrameRef& frame) {
    auto pinned = store->Pin(frame);
    EXPECT_TRUE(pinned.ok()) << pinned.status.detail;
    const uint8_t value = pinned.value.empty() ? 0 : pinned.value[0];
    EXPECT_TRUE(store->Release(frame).ok());
    return value;
  }
};

using Implementations = ::testing::Types<FakeFrameStoreAccessFactory>;
TYPED_TEST_SUITE(FrameStoreAccessContract, Implementations);

TYPED_TEST(FrameStoreAccessContract, AllocateReturnsAFrameMatchingTheRequest) {
  const FrameRef frame = this->Allocate(8, 5);
  EXPECT_EQ(frame.width, 8);
  EXPECT_EQ(frame.height, 5);
  EXPECT_EQ(frame.format, PixelFormat::RGBA8);
  EXPECT_TRUE(frame.id.valid());
}

TYPED_TEST(FrameStoreAccessContract, EachAllocationGetsItsOwnIdentity) {
  EXPECT_NE(this->Allocate().id.value, this->Allocate().id.value);
}

TYPED_TEST(FrameStoreAccessContract, PinYieldsEnoughBytesForTheFrame) {
  const FrameRef frame = this->Allocate(4, 4);
  auto pinned = this->store->Pin(frame);
  ASSERT_TRUE(pinned.ok());
  EXPECT_GE(pinned.value.size(), 4u * 4u * 4u);
  EXPECT_TRUE(this->store->Release(frame).ok());
}

TYPED_TEST(FrameStoreAccessContract, WritesSurviveAReleaseAndRepin) {
  const FrameRef frame = this->Allocate();
  this->Fill(frame, 0xAB);
  EXPECT_EQ(this->FirstByte(frame), 0xAB);
}

TYPED_TEST(FrameStoreAccessContract, PinFaultsASpilledFrameBackIn) {
  // The whole point of spilling: a burst rests on disk and comes back byte-identical when the
  // build finally needs it. If this ever fails, retakes silently rebuild from corrupted input.
  const FrameRef frame = this->Allocate();
  this->Fill(frame, 0x5C);
  ASSERT_TRUE(this->store->Demote(frame, Residency::Spilled).ok());
  EXPECT_EQ(this->FirstByte(frame), 0x5C);
}

TYPED_TEST(FrameStoreAccessContract, ResidencyIsQueriedFromTheStoreNotTheHandle) {
  const FrameRef frame = this->Allocate();
  ASSERT_TRUE(this->store->Demote(frame, Residency::Spilled).ok());
  auto residency = this->store->ResidencyOf(frame);
  ASSERT_TRUE(residency.ok());
  EXPECT_EQ(residency.value, Residency::Spilled);
}

TYPED_TEST(FrameStoreAccessContract, PinPromotesBackToTheHeap) {
  const FrameRef frame = this->Allocate();
  ASSERT_TRUE(this->store->Demote(frame, Residency::Spilled).ok());
  auto pinned = this->store->Pin(frame);
  ASSERT_TRUE(pinned.ok());
  auto residency = this->store->ResidencyOf(frame);
  ASSERT_TRUE(residency.ok());
  EXPECT_EQ(residency.value, Residency::HeapPinned);
  EXPECT_TRUE(this->store->Release(frame).ok());
}

TYPED_TEST(FrameStoreAccessContract, ForgottenFramesAreGone) {
  const FrameRef frame = this->Allocate();
  ASSERT_TRUE(this->store->Forget(frame).ok());
  EXPECT_EQ(this->store->Pin(frame).status.code, StatusCode::NotFound);
  EXPECT_EQ(this->store->ResidencyOf(frame).status.code, StatusCode::NotFound);
}

TYPED_TEST(FrameStoreAccessContract, OperationsOnAnUnknownFrameReportNotFound) {
  FrameRef stranger;
  stranger.id = FrameId{999999};
  EXPECT_EQ(this->store->Pin(stranger).status.code, StatusCode::NotFound);
  EXPECT_EQ(this->store->Demote(stranger, Residency::Spilled).code, StatusCode::NotFound);
  EXPECT_EQ(this->store->ContentHash(stranger).status.code, StatusCode::NotFound);
}

TYPED_TEST(FrameStoreAccessContract, ContentHashDependsOnBytesNotIdentity) {
  // The build graph is keyed on this. Two cells that captured the same pixels must produce the
  // same fingerprint, or a rebuild recomputes work it already has cached.
  const FrameRef a = this->Allocate();
  const FrameRef b = this->Allocate();
  this->Fill(a, 0x11);
  this->Fill(b, 0x11);
  auto ha = this->store->ContentHash(a);
  auto hb = this->store->ContentHash(b);
  ASSERT_TRUE(ha.ok());
  ASSERT_TRUE(hb.ok());
  EXPECT_EQ(ha.value, hb.value);
}

TYPED_TEST(FrameStoreAccessContract, ContentHashChangesWhenThePixelsChange) {
  const FrameRef frame = this->Allocate();
  this->Fill(frame, 0x11);
  const uint64_t before = this->store->ContentHash(frame).value;
  this->Fill(frame, 0x22);
  EXPECT_NE(this->store->ContentHash(frame).value, before);
}

TYPED_TEST(FrameStoreAccessContract, BudgetTracksWhatIsHeldInTheHeap) {
  auto empty = this->store->Budget();
  ASSERT_TRUE(empty.ok());
  EXPECT_GT(empty.value.heapCeilingBytes, 0);

  const FrameRef frame = this->Allocate(16, 16);
  auto used = this->store->Budget();
  ASSERT_TRUE(used.ok());
  EXPECT_GT(used.value.heapUsedBytes, empty.value.heapUsedBytes);

  ASSERT_TRUE(this->store->Forget(frame).ok());
  auto after = this->store->Budget();
  ASSERT_TRUE(after.ok());
  EXPECT_EQ(after.value.heapUsedBytes, empty.value.heapUsedBytes);
}

TYPED_TEST(FrameStoreAccessContract, SpillingMovesBytesOutOfTheHeapBudget) {
  const FrameRef frame = this->Allocate(16, 16);
  const int64_t resident = this->store->Budget().value.heapUsedBytes;
  ASSERT_TRUE(this->store->Demote(frame, Residency::Spilled).ok());

  auto after = this->store->Budget();
  ASSERT_TRUE(after.ok());
  EXPECT_LT(after.value.heapUsedBytes, resident);
  EXPECT_GT(after.value.spilledBytes, 0);
}

TYPED_TEST(FrameStoreAccessContract, ExhaustionIsReportedRatherThanCrashing) {
  // An OOM on a phone is a fatal page crash, so the store has to refuse before it gets there.
  Status last;
  for (int i = 0; i < 10000; ++i) {
    auto r = this->store->Allocate(256, 256, PixelFormat::RGBA8);
    if (!r.ok()) {
      last = r.status;
      break;
    }
  }
  EXPECT_EQ(last.code, StatusCode::FrameStoreExhausted);
}

TYPED_TEST(FrameStoreAccessContract, RejectsNonsenseDimensions) {
  EXPECT_EQ(this->store->Allocate(0, 4, PixelFormat::RGBA8).status.code,
            StatusCode::InvalidArgument);
  EXPECT_EQ(this->store->Allocate(4, 4, PixelFormat::Unknown).status.code,
            StatusCode::InvalidArgument);
}

}  // namespace
}  // namespace sphanorama
