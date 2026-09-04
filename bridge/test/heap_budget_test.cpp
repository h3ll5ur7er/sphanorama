// How much of the heap the frame store may spend, decided from what the device reports rather
// than from a constant in the source.
//
// The failure this guards is not an allocation returning null. A WASM heap that grows past what a
// phone will give a tab is the operating system killing the tab, which no status code reaches —
// so the ceiling has to be low enough to be survivable and high enough that a sphere is worth
// capturing, and the only honest way to sit between those is to read the device.
#include <gtest/gtest.h>

#include <cstdint>

#include "heap_budget.h"

namespace sphanorama::bridge {
namespace {

constexpr int64_t kGiB = 1ll << 30;
constexpr int64_t kMiB = 1ll << 20;

// A linked maximum large enough never to be the binding constraint, so a test about device memory
// is about device memory.
constexpr int64_t kRoomy = 4 * kGiB;

TEST(ChooseHeapCeiling, ScalesWithTheDeviceRatherThanStandingStill) {
  // The whole point. A 1 GB phone and an 8 GB desktop got the same number before this existed,
  // which was too much for one of them and a waste on the other.
  const int64_t small = ChooseHeapCeiling(1 * kGiB, kRoomy);
  const int64_t large = ChooseHeapCeiling(8 * kGiB, kRoomy);
  EXPECT_LT(small, large);
}

TEST(ChooseHeapCeiling, TakesOnlyAFractionOfWhatTheDeviceHas) {
  // A tab's frame store is not entitled to the machine. The rest of the browser, the rest of the
  // page and every other tab are also in that memory, and the one that gets killed for asking too
  // much is this one.
  EXPECT_LT(ChooseHeapCeiling(4 * kGiB, kRoomy), 4 * kGiB / 4);
}

TEST(ChooseHeapCeiling, NeverExceedsWhatTheModuleWasLinkedToAllow) {
  // A ceiling above the build's own maximum could never be reached, so a store that believed it
  // would refuse to spill until the heap had already failed to grow — which is the moment the
  // spill tier exists to arrive before.
  const int64_t linked = 64 * kMiB;
  EXPECT_LE(ChooseHeapCeiling(8 * kGiB, linked), linked);
}

TEST(ChooseHeapCeiling, LeavesRoomInTheLinkedHeapForEverythingThatIsNotAFrame) {
  // The heap also holds the plan, the candidate sets, the allocator's own bookkeeping and every
  // temporary an engine makes. A ceiling equal to the linked maximum would have the store
  // consider itself within budget at the moment malloc starts failing for everyone else.
  const int64_t linked = 256 * kMiB;
  EXPECT_LT(ChooseHeapCeiling(8 * kGiB, linked), linked);
}

TEST(ChooseHeapCeiling, FallsBackToAStatedNumberWhenTheBrowserWillNotSayHowMuchMemoryThereIs) {
  // Safari and Firefox do not report deviceMemory at all, so this is not an edge case — it is
  // every iPhone. The fallback is the number this code used before it could ask, which is the
  // honest choice: no new information means no new answer.
  const int64_t unknown = ChooseHeapCeiling(0, kRoomy);
  EXPECT_EQ(unknown, kStatedCeilingBytes);
}

TEST(ChooseHeapCeiling, StillClampsTheFallbackToTheLinkedHeap) {
  // The fallback is a guess about the device; the linked maximum is a fact about the build. A
  // guess must not survive a fact.
  const int64_t linked = 32 * kMiB;
  EXPECT_LE(ChooseHeapCeiling(0, linked), linked);
}

TEST(ChooseHeapCeiling, KeepsABurstPossibleOnADeviceThatReportsAlmostNothing) {
  // deviceMemory bottoms out at 0.25 GiB, and a sixteenth of that is 16 MB — about two frames.
  // A store that cannot hold one burst cannot capture a cell at all, so the floor is what makes
  // the difference between a degraded capture and no capture.
  const int64_t tiny = ChooseHeapCeiling(kGiB / 4, kRoomy);
  EXPECT_GE(tiny, kFloorCeilingBytes);
}

TEST(ChooseHeapCeiling, TheFloorDoesNotOverrideTheLinkedHeap) {
  // Between a floor that keeps capture possible and a limit that physically cannot be exceeded,
  // the limit wins: returning the floor here would be a store that believes in memory the module
  // can never address.
  const int64_t linked = kFloorCeilingBytes / 2;
  EXPECT_LE(ChooseHeapCeiling(kGiB / 4, linked), linked);
}

TEST(ChooseHeapCeiling, ATinyLinkedHeapIsAFactRatherThanASilence) {
  // Zero is the sentinel for "the platform did not answer", and it was computed from the *share*
  // of the linked maximum rather than from the maximum itself — so a linked heap of a few bytes
  // divided down to zero, was read as silence, and returned a ceiling millions of times larger
  // than the heap it was supposed to be clamped by. A number is a number however small; only the
  // absence of one is silence.
  EXPECT_LE(ChooseHeapCeiling(8 * kGiB, 3), 3);
}

TEST(ChooseHeapCeiling, IsNeverNegativeOrZeroHoweverNonsensicalTheInputs) {
  // Both numbers come from the browser through a boundary that has been wrong before. A ceiling
  // of zero would make the store refuse every allocation, which reads as a broken camera.
  EXPECT_GT(ChooseHeapCeiling(-1, kRoomy), 0);
  EXPECT_GT(ChooseHeapCeiling(kGiB, -1), 0);
  EXPECT_GT(ChooseHeapCeiling(0, 0), 0);
}

TEST(ChooseHeapCeiling, IsDeterministicForTheSameDevice) {
  // The ceiling is read into FrameStoreBudget and shown to the user; a number that moved between
  // reads would make the same session look like two.
  EXPECT_EQ(ChooseHeapCeiling(4 * kGiB, kRoomy), ChooseHeapCeiling(4 * kGiB, kRoomy));
}

}  // namespace
}  // namespace sphanorama::bridge
