// The wire primitives. Everything above them is generated, so a bug here is a bug in every
// message — and the reader has to survive a payload that is truncated, hostile, or both.
#include <gtest/gtest.h>

#include "sphanorama/wire.h"

namespace sphanorama::wire {
namespace {

Reader ReaderOver(const Writer& writer) {
  return Reader(writer.bytes().data(), writer.bytes().size());
}

TEST(Wire, RoundTripsEveryScalarKind) {
  Writer w;
  w.PutBool(true);
  w.PutI32(-42);
  w.PutU64(0xFEEDFACEDEADBEEFull);
  w.PutF64(3.5);
  w.PutString("hello");

  Reader r = ReaderOver(w);
  EXPECT_TRUE(r.GetBool());
  EXPECT_EQ(r.GetI32(), -42);
  EXPECT_EQ(r.GetU64(), 0xFEEDFACEDEADBEEFull);
  EXPECT_DOUBLE_EQ(r.GetF64(), 3.5);
  EXPECT_EQ(r.GetString(), "hello");
  EXPECT_TRUE(r.ok());
}

TEST(Wire, PreservesEveryBitOfA64BitHash) {
  // The build graph is keyed on content hashes. Widening one through a double would drop the low
  // bits, and a stale stage would be reused with nothing failing.
  const uint64_t hash = 0x0123456789ABCDEFull;
  Writer w;
  w.PutU64(hash);
  EXPECT_EQ(ReaderOver(w).GetU64(), hash);
}

TEST(Wire, HandlesEmptyStringsAndBytes) {
  Writer w;
  w.PutString("");
  w.PutBytes({});
  Reader r = ReaderOver(w);
  EXPECT_EQ(r.GetString(), "");
  EXPECT_TRUE(r.GetBytes().empty());
  EXPECT_TRUE(r.ok());
}

TEST(Wire, RoundTripsBinaryPayloads) {
  const std::vector<uint8_t> payload{0, 255, 7, 0, 128};
  Writer w;
  w.PutBytes(payload);
  EXPECT_EQ(ReaderOver(w).GetBytes(), payload);
}

TEST(Wire, ReadingPastTheEndFailsRatherThanReadingGarbage) {
  Writer w;
  w.PutI32(1);
  Reader r = ReaderOver(w);
  EXPECT_EQ(r.GetI32(), 1);
  EXPECT_TRUE(r.ok());
  (void)r.GetI32();
  EXPECT_FALSE(r.ok());
}

TEST(Wire, ATruncatedStringIsRejected) {
  // A length prefix that outruns the payload is what a truncated postMessage looks like.
  Writer w;
  w.PutI32(64);          // claims 64 bytes
  Reader r(w.bytes().data(), w.bytes().size());
  EXPECT_TRUE(r.GetString().empty());
  EXPECT_FALSE(r.ok());
}

TEST(Wire, ANegativeLengthIsRejected) {
  Writer w;
  w.PutI32(-1);
  Reader r(w.bytes().data(), w.bytes().size());
  (void)r.GetString();
  EXPECT_FALSE(r.ok());
}

TEST(Wire, AnAbsurdCountIsRejectedBeforeAnythingIsAllocated) {
  // One corrupt length prefix must not become a multi-gigabyte allocation. On a phone that is
  // not an exception, it is the tab dying.
  Writer w;
  w.PutI32(1'000'000'000);
  Reader r(w.bytes().data(), w.bytes().size());
  EXPECT_EQ(r.GetCount(4), 0u);
  EXPECT_FALSE(r.ok());
}

TEST(Wire, OnceFailedEveryFurtherReadIsANoOp) {
  // Callers check ok() once at the end rather than after every field, so a failed reader must
  // stay failed and keep returning zeroes instead of resynchronising onto garbage.
  Reader r(nullptr, 0);
  (void)r.GetI32();
  ASSERT_FALSE(r.ok());
  EXPECT_EQ(r.GetI32(), 0);
  EXPECT_EQ(r.GetString(), "");
  EXPECT_FALSE(r.ok());
}

TEST(Wire, AnEmptyPayloadIsSafeToRead) {
  Reader r(nullptr, 0);
  EXPECT_FALSE(r.GetBool());
  EXPECT_FALSE(r.ok());
}

TEST(Wire, CountsAreCheckedAgainstBytesRemaining) {
  Writer w;
  w.PutCount(3);
  w.PutI32(1);
  Reader r = ReaderOver(w);
  EXPECT_EQ(r.GetCount(4), 0u);   // claims 3 elements of 4 bytes, only 4 bytes follow
  EXPECT_FALSE(r.ok());
}

}  // namespace
}  // namespace sphanorama::wire
