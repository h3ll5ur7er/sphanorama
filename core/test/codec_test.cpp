// The generated codec.
//
// A round trip inside one language proves the emitter is self-consistent, which is the weaker
// half. The stronger half is the golden payload: a byte-for-byte constant that the TypeScript
// suite decodes too, so the two generated halves are pinned to each other rather than merely to
// their own emitter.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <sstream>

#include "sphanorama/codec.h"

namespace sphanorama::codec {
namespace {

std::string Hex(const std::vector<uint8_t>& bytes) {
  std::ostringstream out;
  for (const uint8_t byte : bytes) {
    out << "0123456789abcdef"[byte >> 4] << "0123456789abcdef"[byte & 0x0F];
  }
  return out.str();
}

template <typename T>
T RoundTrip(const T& value) {
  Writer writer;
  Encode(writer, value);
  Reader reader(writer.bytes().data(), writer.bytes().size());
  T decoded{};
  EXPECT_TRUE(Decode(reader, decoded));
  return decoded;
}

TEST(Codec, RoundTripsAFlatStruct) {
  CaptureGuidance guidance;
  guidance.targetNode = NodeId{7};
  guidance.angularErrorDeg = 12.5;
  guidance.rollErrorDeg = -3.25;
  guidance.stability = 0.5;
  guidance.action = GuidanceAction::HoldStill;

  const CaptureGuidance decoded = RoundTrip(guidance);
  EXPECT_EQ(decoded.targetNode.value, 7u);
  EXPECT_DOUBLE_EQ(decoded.angularErrorDeg, 12.5);
  EXPECT_DOUBLE_EQ(decoded.rollErrorDeg, -3.25);
  EXPECT_EQ(decoded.action, GuidanceAction::HoldStill);
}

TEST(Codec, RoundTripsNestedStructsAndVectors) {
  CapturePlan plan;
  plan.spec.acceptanceConeDeg = 4.0;
  plan.spec.strategy = TessellationStrategy::Geodesic;
  for (uint64_t i = 1; i <= 3; ++i) {
    CoverageNode node;
    node.id = NodeId{i};
    node.acceptanceConeDeg = static_cast<double>(i);
    node.ringIndex = static_cast<int32_t>(i);
    plan.nodes.push_back(node);
  }

  const CapturePlan decoded = RoundTrip(plan);
  ASSERT_EQ(decoded.nodes.size(), 3u);
  EXPECT_EQ(decoded.nodes[2].id.value, 3u);
  EXPECT_DOUBLE_EQ(decoded.nodes[1].acceptanceConeDeg, 2.0);
  EXPECT_EQ(decoded.spec.strategy, TessellationStrategy::Geodesic);
}

TEST(Codec, RoundTripsAnEmptyVector) {
  CoverageState state;
  state.nodesTotal = 4;
  const CoverageState decoded = RoundTrip(state);
  EXPECT_EQ(decoded.nodesTotal, 4);
  EXPECT_TRUE(decoded.holes.empty());
}

TEST(Codec, RoundTripsStrings) {
  ProjectSummary summary;
  summary.id = ProjectId{2};
  summary.title = "kitchen · 全景";
  EXPECT_EQ(RoundTrip(summary).title, "kitchen · 全景");
}

TEST(Codec, PreservesA64BitContentHash) {
  // The build graph is keyed on this. Losing the low bits would silently reuse a stale stage.
  FrameRef frame;
  frame.contentHash = 0x0123456789ABCDEFull;
  EXPECT_EQ(RoundTrip(frame).contentHash, 0x0123456789ABCDEFull);
}

TEST(Codec, ATruncatedPayloadIsRejectedRatherThanPartiallyDecoded) {
  CapturePlan plan;
  plan.nodes.resize(2);
  Writer writer;
  Encode(writer, plan);

  std::vector<uint8_t> truncated = writer.bytes();
  truncated.resize(truncated.size() / 2);

  Reader reader(truncated.data(), truncated.size());
  CapturePlan decoded{};
  EXPECT_FALSE(Decode(reader, decoded));
}

TEST(Codec, TheWireFormatIsPinnedAcrossLanguages) {
  // This exact hex is decoded by shell/src/bridge/codec.test.ts. If the two generated halves
  // ever disagree about field order or widths, one of the two tests fails here rather than a
  // payload decoding into plausible nonsense in a browser.
  CaptureGuidance guidance;
  guidance.targetNode = NodeId{7};
  guidance.angularErrorDeg = 12.5;
  guidance.rollErrorDeg = -3.25;
  guidance.stability = 0.5;
  guidance.action = GuidanceAction::HoldStill;

  Writer writer;
  Encode(writer, guidance);
  EXPECT_EQ(Hex(writer.bytes()),
            "0000000000001c4000000000000029400000000000000ac0000000000000e03f01000000");
}

TEST(Codec, RoundTripsABytePayload) {
  // The first field in these contracts that is bytes rather than numbers, and it carries the one
  // thing the boundary otherwise refuses to move: pixels (ADR 0038). A byte sequence is
  // length-prefixed, so it is also the one field where a disagreement about the prefix decodes as
  // a shorter image rather than as a failure.
  FramePreview preview;
  preview.frame = FrameId{4};
  preview.width = 2;
  preview.height = 2;
  preview.format = PixelFormat::RGBA8;
  preview.pixels.assign(2 * 2 * 4, 0u);
  preview.pixels.front() = 200u;
  preview.pixels.back() = 255u;

  const FramePreview decoded = RoundTrip(preview);
  EXPECT_EQ(decoded.frame.value, 4u);
  EXPECT_EQ(decoded.width, 2);
  EXPECT_EQ(decoded.height, 2);
  EXPECT_EQ(decoded.format, PixelFormat::RGBA8);
  ASSERT_EQ(decoded.pixels.size(), preview.pixels.size());
  EXPECT_EQ(decoded.pixels, preview.pixels);
}

TEST(Codec, TheByteFormatIsPinnedAcrossLanguagesToo) {
  // The same discipline as the guidance golden above, over the one field kind that carries a
  // length prefix. A prefix width or endianness the two halves disagreed about would decode into
  // a plausible image of the wrong size rather than failing.
  FramePreview preview;
  preview.frame = FrameId{9};
  preview.width = 2;
  preview.height = 1;
  preview.format = PixelFormat::RGBA8;
  preview.pixels = {1u, 2u, 3u, 255u, 4u, 5u, 6u, 255u};

  Writer writer;
  Encode(writer, preview);
  EXPECT_EQ(Hex(writer.bytes()),
            "00000000000022400000000000000040000000000000f03f0100000008000000010203ff040506ff");
}

TEST(Codec, AnIntegerFieldThatIsNotAnIntegerFailsTheDecode) {
  // Every number crosses as a double, because JavaScript has no other kind, so an `int32_t` field
  // arrives as one and has to become an integer again. `static_cast<int32_t>` of a NaN, an
  // infinity or a 1e300 is undefined — the far side can present all three, whether through a bug,
  // a hostile page or a document written by another build — and the codec used to do exactly that.
  //
  // The bytes are made by the encoder and then one field is overwritten, rather than assembled by
  // hand, so this does not encode a second copy of the field order and cannot rot when the struct
  // gains a member.
  FrameRef frame;
  frame.id = FrameId{1};
  frame.buffer = BufferId{1};
  frame.format = PixelFormat::RGBA8;
  frame.width = 24680;          // a value nothing else in this struct holds
  frame.height = 8;
  frame.stride = 32;

  Writer writer;
  Encode(writer, frame);
  const std::vector<uint8_t> good = writer.bytes();

  const double sentinel = 24680.0;
  std::vector<uint8_t> pattern(sizeof(double));
  std::memcpy(pattern.data(), &sentinel, sizeof(double));
  const auto at = std::search(good.begin(), good.end(), pattern.begin(), pattern.end());
  ASSERT_NE(at, good.end()) << "the width is not in the payload, so this test found nothing to bend";

  for (const double raw : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity(), 1e300, 0.5}) {
    std::vector<uint8_t> bent = good;
    std::memcpy(bent.data() + (at - good.begin()), &raw, sizeof(double));
    Reader reader(bent.data(), bent.size());
    FrameRef decoded{};
    EXPECT_FALSE(Decode(reader, decoded)) << "a width of " << raw << " decoded into something";
  }

  // And the untouched payload still decodes, or the assertions above would pass against a codec
  // that had simply stopped working.
  Reader reader(good.data(), good.size());
  FrameRef decoded{};
  ASSERT_TRUE(Decode(reader, decoded));
  EXPECT_EQ(decoded.width, frame.width);
}

}  // namespace
}  // namespace sphanorama::codec
