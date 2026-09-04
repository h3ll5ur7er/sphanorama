// The frame-store contract, as a suite that runs against every implementation.
//
// Only the in-memory store is listed today. When the OPFS-backed browser store arrives it joins
// the type list below, and the property that matters — that all implementations agree — is
// checked by construction rather than by reading two files side by side.
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

#include "sphanorama/resource_access/frame_store_access.h"
#include "support/frame_store_factory.h"

namespace sphanorama {
namespace {

template <typename Factory>
class FrameStoreAccessContract : public ::testing::Test {
 protected:
  // Declared before the store, so it outlives it: the store holds a pointer, not ownership, and
  // member destruction runs in reverse.
  typename Factory::Sink sink;
  std::unique_ptr<IFrameStoreAccess> store = Factory::Create(&sink);

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

using Implementations = ::testing::Types<MemoryFrameStoreAccessFactory>;
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

TYPED_TEST(FrameStoreAccessContract, FaultingInIsRefusedWhenTheHeapHasNoRoomLeft) {
  // Spilling frees budget, and something else takes it. Promoting on Pin without re-checking the
  // ceiling is how a store that models mobile memory pressure quietly stops modelling it: the
  // manager tests keep passing while the phone is over its budget.
  const FrameRef spilled = this->Allocate();
  ASSERT_TRUE(this->store->Demote(spilled, Residency::Spilled).ok());

  auto budget = this->store->Budget();
  ASSERT_TRUE(budget.ok());
  std::vector<FrameRef> hogs;
  while (true) {
    auto more = this->store->Allocate(4, 4, PixelFormat::RGBA8);
    if (!more.ok()) break;
    hogs.push_back(more.value);
  }

  EXPECT_EQ(this->store->Pin(spilled).status.code, StatusCode::FrameStoreExhausted);
  auto after = this->store->Budget();
  ASSERT_TRUE(after.ok());
  EXPECT_LE(after.value.heapUsedBytes, after.value.heapCeilingBytes);
}

TYPED_TEST(FrameStoreAccessContract, ForgettingAPinnedFrameIsRefused) {
  // Pin hands out a span and promises it stays valid until Release. Forget erasing the entry
  // underneath that promise is a use-after-free for whoever is still holding the span — and the
  // manager's rollback paths call Forget on frames it may not have released yet, so this is a
  // lifetime rule every implementation has to keep, not a detail of one.
  const FrameRef frame = this->Allocate();
  auto pinned = this->store->Pin(frame);
  ASSERT_TRUE(pinned.ok());

  EXPECT_EQ(this->store->Forget(frame).code, StatusCode::FailedPrecondition);
  // Still there, and still the same bytes: a refused Forget must not half-destroy anything.
  EXPECT_TRUE(this->store->ResidencyOf(frame).ok());

  ASSERT_TRUE(this->store->Release(frame).ok());
  EXPECT_TRUE(this->store->Forget(frame).ok());
}

TYPED_TEST(FrameStoreAccessContract, AdoptsAFrameSpilledByAStoreThatIsGone) {
  // The reload. A capture's frames end up in the sink when their cell is committed (ADR 0023),
  // and the store that put them there dies with the tab — so the only thing left naming them is
  // the session document, and coming back means handing those names to a store that never
  // allocated them.
  auto frame = this->Allocate();
  this->Fill(frame, 0xAB);
  ASSERT_TRUE(this->store->Demote(frame, Residency::Spilled).ok());

  std::unique_ptr<IFrameStoreAccess> revived = TypeParam::Create(&this->sink);
  ASSERT_TRUE(revived->Adopt(frame).ok());

  // Spilled, not resident: adopting is a promise about where the bytes are, and a store that
  // called them heap-resident would be charging a budget for memory it does not hold.
  auto residency = revived->ResidencyOf(frame);
  ASSERT_TRUE(residency.ok()) << residency.status.detail;
  EXPECT_EQ(residency.value, Residency::Spilled);

  auto pinned = revived->Pin(frame);
  ASSERT_TRUE(pinned.ok()) << pinned.status.detail;
  ASSERT_FALSE(pinned.value.empty());
  EXPECT_EQ(pinned.value[0], 0xAB);
  EXPECT_TRUE(revived->Release(frame).ok());
}

TYPED_TEST(FrameStoreAccessContract, AdoptingAnIdentityTheStoreAlreadyHoldsIsRefused) {
  // Two frames under one identity is a store that hands the wrong pixels to whoever asks second,
  // and a resume replaying a document twice is exactly how that would arrive.
  auto frame = this->Allocate();
  EXPECT_FALSE(this->store->Adopt(frame).ok());
}

TYPED_TEST(FrameStoreAccessContract, AnAdoptedIdentityIsNeverHandedOutAgain) {
  // A resumed session keeps capturing, and its new frames come from the same counter that issued
  // the restored ones. A store that started again from where it left off would collide with the
  // document it had just been handed.
  // Exactly the identity this store would issue next, which is the collision that actually
  // happens: the document was written by a store that counted the same way from the same start.
  FrameRef restored = this->Allocate();
  restored.id = FrameId{restored.id.value + 1};
  restored.buffer = BufferId{restored.id.value};
  ASSERT_TRUE(this->store->Adopt(restored).ok());

  for (int i = 0; i < 3; ++i) {
    EXPECT_NE(this->Allocate().id.value, restored.id.value) << "allocation " << i;
  }
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

TYPED_TEST(FrameStoreAccessContract, RefusesToDemoteToATierADemotionCannotProduce) {
  // A store that accepted any Residency would have ResidencyOf reporting a state that was never
  // established. Pinned is the one that bites: it claimed a mapping nothing had made, and the
  // Release that followed then failed against a pin count of zero.
  const FrameRef frame = this->Allocate();
  EXPECT_EQ(this->store->Demote(frame, Residency::HeapPinned).code, StatusCode::InvalidArgument);
  EXPECT_EQ(this->store->Demote(frame, Residency::GpuTexture).code, StatusCode::Unsupported);

  // Refused *and* unchanged: a rejected demotion that had already moved the frame would be worse
  // than one that succeeded.
  auto residency = this->store->ResidencyOf(frame);
  ASSERT_TRUE(residency.ok());
  EXPECT_EQ(residency.value, Residency::HeapEncoded);
  EXPECT_TRUE(this->store->Pin(frame).ok()) << "the refused demotion left the frame unusable";
  EXPECT_TRUE(this->store->Release(frame).ok());
}

// Not part of the typed suite because it needs a ceiling large enough for the size check to pass
// so the *stride* check is the one that answers, and the suite's store is deliberately small so
// its exhaustion cases stay reachable. Nothing is allocated: the refusal comes first.
TEST(MemoryFrameStoreAllocation, RefusesAFrameWhoseRowCannotBeDescribed) {
  // FrameByteSize guards its own arithmetic in int64, and the stride line next to it did not:
  // `width * BytesPerPixel(format)` is int32 * int32, so a width past INT32_MAX/4 overflowed
  // after passing every check above it. Signed overflow is undefined behaviour rather than a
  // wrong number the caller could sanity-check.
  // A ceiling of a terabyte, so the 2.4 GB this asks for clears the exhaustion check and the
  // stride check is the only one left that can answer. That is what makes the code below a
  // discriminator: without the range check the allocation reaches the vector, and with the
  // ceiling small it would have been refused for the wrong reason and proved nothing.
  //
  // The other side of the boundary is deliberately not tested: any width whose row *just* fits an
  // int32 stride is a two-gigabyte allocation, and a test is not the place to find out whether
  // the machine has it.
  MemoryFrameStoreAccess store(1LL << 40);
  auto refused = store.Allocate(600'000'000, 1, PixelFormat::RGBA8);
  EXPECT_EQ(refused.status.code, StatusCode::InvalidArgument) << refused.status.detail;

  // Nothing was allocated on the way to refusing.
  auto budget = store.Budget();
  ASSERT_TRUE(budget.ok());
  EXPECT_EQ(budget.value.heapUsedBytes, 0);
}

}  // namespace
}  // namespace sphanorama
