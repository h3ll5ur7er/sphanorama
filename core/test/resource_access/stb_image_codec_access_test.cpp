// The codec an export encodes through (ADR 0087): a frame from the store in, a JPEG out, tagged as
// a photo sphere when asked. Checked by decoding the file it wrote, with a decoder that is not the
// encoder, rather than by trusting the call.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "resource_access/image_codec_access/stb_image_codec_access.h"
#include "support/fake_spill_sink.h"
#include "support/recording_frame_store_access.h"

#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace sphanorama {
namespace {

class StbImageCodec : public ::testing::Test {
 protected:
  // A picture with structure in both directions, so a transposed or mirrored write shows.
  FrameRef Picture(int32_t width, int32_t height) {
    auto frame = store.Allocate(width, height, PixelFormat::RGBA8);
    EXPECT_TRUE(frame.ok());
    auto pinned = store.Pin(frame.value);
    EXPECT_TRUE(pinned.ok());
    for (int32_t y = 0; y < height; ++y) {
      for (int32_t x = 0; x < width; ++x) {
        uint8_t* px = pinned.value.data() + static_cast<size_t>(y) * frame.value.stride +
                      static_cast<size_t>(x) * 4;
        px[0] = static_cast<uint8_t>(x * 255 / (width - 1));
        px[1] = static_cast<uint8_t>(y * 255 / (height - 1));
        px[2] = 96;
        px[3] = 255;
      }
    }
    EXPECT_TRUE(store.Release(frame.value).ok());
    return frame.value;
  }

  EncodeSpec Jpeg(int32_t quality = 92) {
    EncodeSpec spec;
    spec.format = EncodeFormat::Jpeg;
    spec.quality = quality;
    return spec;
  }

  // The mean absolute difference per channel between the frame and the file, as decoded.
  double MeanError(const FrameRef& frame, const std::vector<uint8_t>& jpeg) {
    int w = 0, h = 0, comp = 0;
    stbi_uc* decoded = stbi_load_from_memory(jpeg.data(), static_cast<int>(jpeg.size()), &w, &h,
                                             &comp, 3);
    EXPECT_NE(decoded, nullptr) << stbi_failure_reason();
    if (decoded == nullptr) return 1e9;
    EXPECT_EQ(w, frame.width);
    EXPECT_EQ(h, frame.height);
    auto pinned = store.Pin(frame);
    EXPECT_TRUE(pinned.ok());
    double sum = 0;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        for (int c = 0; c < 3; ++c) {
          const int want = pinned.value[static_cast<size_t>(y) * frame.stride +
                                        static_cast<size_t>(x) * 4 + static_cast<size_t>(c)];
          const int got = decoded[(static_cast<size_t>(y) * w + static_cast<size_t>(x)) * 3 +
                                  static_cast<size_t>(c)];
          sum += std::abs(want - got);
        }
      }
    }
    EXPECT_TRUE(store.Release(frame).ok());
    stbi_image_free(decoded);
    return sum / (static_cast<double>(w) * h * 3);
  }

  static bool Contains(const std::vector<uint8_t>& bytes, const std::string& text) {
    return std::search(bytes.begin(), bytes.end(), text.begin(), text.end()) != bytes.end();
  }

  // The XMP packet as a reader finds it: walking the segments from the start of image to the start
  // of scan, in an APP1 that opens with the XMP namespace. Text anywhere else in the file is not
  // metadata a viewer reads.
  static std::optional<std::string> XmpPacketIn(const std::vector<uint8_t>& jpeg) {
    const std::string xmp = std::string("http://ns.adobe.com/xap/1.0/") + '\0';
    if (jpeg.size() < 2 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) return std::nullopt;
    size_t at = 2;
    while (at + 4 <= jpeg.size() && jpeg[at] == 0xFF && jpeg[at + 1] != 0xDA) {
      const size_t length = (static_cast<size_t>(jpeg[at + 2]) << 8) | jpeg[at + 3];
      if (length < 2 || at + 2 + length > jpeg.size()) return std::nullopt;
      const auto body = jpeg.begin() + static_cast<std::ptrdiff_t>(at + 4);
      const auto end = jpeg.begin() + static_cast<std::ptrdiff_t>(at + 2 + length);
      if (jpeg[at + 1] == 0xE1 && static_cast<size_t>(end - body) >= xmp.size() &&
          std::equal(xmp.begin(), xmp.end(), body)) {
        return std::string(body + static_cast<std::ptrdiff_t>(xmp.size()), end);
      }
      at += 2 + length;
    }
    return std::nullopt;
  }

  static void ExpectAttribute(const std::optional<std::string>& packet, const std::string& name,
                              const std::string& value) {
    ASSERT_TRUE(packet.has_value()) << "no XMP segment before the image data";
    const std::string written = "GPano:" + name + "=\"" + value + "\"";
    EXPECT_NE(packet->find(written), std::string::npos) << written << " in\n" << *packet;
  }

  FakeSpillSink sink;
  MemoryFrameStoreAccess store{1 << 22, &sink};
  StbImageCodecAccess codec{store};
};

TEST_F(StbImageCodec, AFrameIsWrittenAsAJpegOfThatPicture) {
  const FrameRef frame = Picture(64, 32);
  auto jpeg = codec.Encode(frame, Jpeg(95));
  ASSERT_TRUE(jpeg.ok()) << jpeg.status.detail;
  ASSERT_GE(jpeg.value.size(), 4u);
  EXPECT_EQ(jpeg.value[0], 0xFF);
  EXPECT_EQ(jpeg.value[1], 0xD8);
  EXPECT_EQ(jpeg.value[jpeg.value.size() - 2], 0xFF);
  EXPECT_EQ(jpeg.value[jpeg.value.size() - 1], 0xD9);
  // A smooth picture at high quality decodes to within a couple of levels of itself; a transposed,
  // mirrored or channel-swapped write is off by tens.
  EXPECT_LT(MeanError(frame, jpeg.value), 3.0);
  EXPECT_FALSE(XmpPacketIn(jpeg.value).has_value()) << "tagged as a sphere without being asked";
  EXPECT_FALSE(Contains(jpeg.value, "GPano"));
}

TEST_F(StbImageCodec, QualityIsTheEncodersAndOutsideOneToAHundredIsRefused) {
  const FrameRef frame = Picture(64, 32);
  auto rough = codec.Encode(frame, Jpeg(10));
  auto fine = codec.Encode(frame, Jpeg(100));
  ASSERT_TRUE(rough.ok() && fine.ok());
  EXPECT_LT(rough.value.size(), fine.value.size());
  EXPECT_GT(MeanError(frame, rough.value), MeanError(frame, fine.value));
  for (const int32_t quality : {0, 101, -5}) {
    EXPECT_EQ(codec.Encode(frame, Jpeg(quality)).status.code, StatusCode::InvalidArgument)
        << quality;
  }
}

// A frame that is the whole sphere, tagged so a viewer opens it as one, and still a JPEG that
// decodes to the same picture.
TEST_F(StbImageCodec, ASphereIsTaggedWithGPanoAndStillDecodes) {
  const FrameRef frame = Picture(64, 32);
  EncodeSpec spec = Jpeg(95);
  spec.attachGPanoXmp = true;
  auto jpeg = codec.Encode(frame, spec);
  ASSERT_TRUE(jpeg.ok()) << jpeg.status.detail;
  const std::optional<std::string> packet = XmpPacketIn(jpeg.value);
  ExpectAttribute(packet, "ProjectionType", "equirectangular");
  ExpectAttribute(packet, "FullPanoWidthPixels", "64");
  ExpectAttribute(packet, "FullPanoHeightPixels", "32");
  ExpectAttribute(packet, "CroppedAreaImageWidthPixels", "64");
  ExpectAttribute(packet, "CroppedAreaImageHeightPixels", "32");
  EXPECT_LT(MeanError(frame, jpeg.value), 3.0);
}

// A band about the horizon, in a sphere the spec names: placed centred, as a capture with no caps
// is, and refused where it cannot be part of the sphere named.
TEST_F(StbImageCodec, ABandIsPlacedAboutTheHorizonOfTheSphereNamed) {
  // Twenty rows of thirty-two, so the band's top, (32 - 20) / 2, is no other simple sum of them.
  const FrameRef frame = Picture(64, 20);
  EncodeSpec spec = Jpeg();
  spec.attachGPanoXmp = true;
  spec.fullPanoWidth = 64;
  spec.fullPanoHeight = 32;
  auto jpeg = codec.Encode(frame, spec);
  ASSERT_TRUE(jpeg.ok()) << jpeg.status.detail;
  const std::optional<std::string> packet = XmpPacketIn(jpeg.value);
  ExpectAttribute(packet, "CroppedAreaTopPixels", "6");
  ExpectAttribute(packet, "CroppedAreaImageWidthPixels", "64");
  ExpectAttribute(packet, "CroppedAreaImageHeightPixels", "20");
  ExpectAttribute(packet, "CroppedAreaLeftPixels", "0");
  ExpectAttribute(packet, "FullPanoHeightPixels", "32");
  // Of a frame that is not a sphere by itself, with no sphere named: nothing to place it in.
  spec.fullPanoWidth = 0;
  spec.fullPanoHeight = 0;
  EXPECT_EQ(codec.Encode(frame, spec).status.code, StatusCode::InvalidArgument);
  spec.fullPanoWidth = 32;
  spec.fullPanoHeight = 16;
  EXPECT_EQ(codec.Encode(frame, spec).status.code, StatusCode::InvalidArgument)
      << "a sphere smaller than the frame";
  // A band goes all the way round: a frame narrower than the sphere has no azimuth anybody gave.
  const FrameRef half = Picture(32, 16);
  spec.fullPanoWidth = 64;
  spec.fullPanoHeight = 32;
  EXPECT_EQ(codec.Encode(half, spec).status.code, StatusCode::InvalidArgument);
  // And a sphere named by one size only is none.
  spec.fullPanoWidth = 64;
  spec.fullPanoHeight = 0;
  EXPECT_EQ(codec.Encode(frame, spec).status.code, StatusCode::InvalidArgument);
}

TEST_F(StbImageCodec, WhatThisCodecDoesNotWriteIsUnsupported) {
  const FrameRef frame = Picture(16, 8);
  for (const EncodeFormat format : {EncodeFormat::Avif, EncodeFormat::Png}) {
    EncodeSpec spec = Jpeg();
    spec.format = format;
    EXPECT_EQ(codec.Encode(frame, spec).status.code, StatusCode::Unsupported);
  }
  const std::vector<uint8_t> bytes{0xFF, 0xD8, 0xFF, 0xD9};
  EXPECT_EQ(codec.Decode(bytes).status.code, StatusCode::Unsupported);
}

// Rows padded past the frame's width are packed before they are written: a writer handed the
// padded rows as they are would read each row from the wrong place, and shear the picture.
TEST_F(StbImageCodec, PaddedRowsAreWrittenAsThePicture) {
  const FrameRef whole = Picture(32, 16);
  FrameRef narrower = whole;
  narrower.width = 24;
  ASSERT_GT(narrower.stride, narrower.width * 4) << "the premise: rows padded past the width";
  auto jpeg = codec.Encode(narrower, Jpeg(95));
  ASSERT_TRUE(jpeg.ok()) << jpeg.status.detail;
  EXPECT_LT(MeanError(narrower, jpeg.value), 3.0);
}

// A handle is the caller's account of a frame, and the store's entry is the only thing that knows
// the allocation: a claim it cannot back is refused before a byte is read.
TEST_F(StbImageCodec, AHandleTheStoreCannotBackIsRefused) {
  const FrameRef frame = Picture(16, 8);
  FrameRef taller = frame;
  taller.height = 4096;
  EXPECT_EQ(codec.Encode(taller, Jpeg()).status.code, StatusCode::InvalidArgument);
  // One row, so only the row itself can be past what is held.
  FrameRef wider = frame;
  wider.width = 4096;
  wider.height = 1;
  wider.stride = 0;
  EXPECT_EQ(codec.Encode(wider, Jpeg()).status.code, StatusCode::InvalidArgument);
  FrameRef sheared = frame;
  sheared.stride = 16;
  EXPECT_EQ(codec.Encode(sheared, Jpeg()).status.code, StatusCode::InvalidArgument);
  FrameRef gray = frame;
  gray.format = PixelFormat::Gray8;
  EXPECT_EQ(codec.Encode(gray, Jpeg()).status.code, StatusCode::InvalidArgument);
  FrameRef empty = frame;
  empty.width = 0;
  EXPECT_EQ(codec.Encode(empty, Jpeg()).status.code, StatusCode::InvalidArgument);
  FrameRef unknown = frame;
  unknown.id = FrameId{frame.id.value + 1000};
  const Status missing = codec.Encode(unknown, Jpeg()).status;
  EXPECT_FALSE(missing.ok());
  EXPECT_NE(missing.code, StatusCode::InvalidArgument) << "the store's own answer, not a guess";
}

// A JPEG's sides are sixteen bits, and stb writes them without asking: a side past 65535 would be
// a smaller picture under the same name. Refused before the writer sees it.
TEST_F(StbImageCodec, ASideAJpegCannotHoldIsRefused) {
  const FrameRef frame = Picture(256, 256);
  FrameRef wide = frame;
  wide.width = 65536;
  wide.height = 1;
  wide.stride = 0;
  EXPECT_EQ(codec.Encode(wide, Jpeg()).status.code, StatusCode::InvalidArgument)
      << "the bytes are there; the format cannot say the width";
  FrameRef tall = frame;
  tall.width = 1;
  tall.height = 65536;
  tall.stride = 4;
  EXPECT_EQ(codec.Encode(tall, Jpeg()).status.code, StatusCode::InvalidArgument);
  // And 65535 is written, at the size it says: the limit is the format's, not a margin under it.
  for (const auto& [width, height, stride] : {std::tuple{65535, 1, 0}, std::tuple{1, 65535, 4}}) {
    FrameRef longest = frame;
    longest.width = width;
    longest.height = height;
    longest.stride = stride;
    auto jpeg = codec.Encode(longest, Jpeg());
    ASSERT_TRUE(jpeg.ok()) << width << "x" << height << ": " << jpeg.status.detail;
    int w = 0, h = 0, comp = 0;
    ASSERT_EQ(stbi_info_from_memory(jpeg.value.data(), static_cast<int>(jpeg.value.size()), &w,
                                    &h, &comp),
              1);
    EXPECT_EQ(w, width);
    EXPECT_EQ(h, height);
  }
}

// The size a build hands across (ADR 0071's 2048 by 1024 preview), through this codec rather than
// the fake the manager tests use: a limit set below it would refuse every export the page makes.
TEST(StbImageCodecLimits, APreviewAsLargeAsABuildMakesIsWritten) {
  MemoryFrameStoreAccess large{int64_t{1} << 24};
  StbImageCodecAccess codec(large);
  auto frame = large.Allocate(2048, 1024, PixelFormat::RGBA8);
  ASSERT_TRUE(frame.ok());
  EncodeSpec spec;
  spec.attachGPanoXmp = true;
  auto jpeg = codec.Encode(frame.value, spec);
  ASSERT_TRUE(jpeg.ok()) << jpeg.status.detail;
  int w = 0, h = 0, comp = 0;
  ASSERT_EQ(stbi_info_from_memory(jpeg.value.data(), static_cast<int>(jpeg.value.size()), &w, &h,
                                  &comp),
            1);
  EXPECT_EQ(w, 2048);
  EXPECT_EQ(h, 1024);
}

// A store that says it holds far more than it does, so a frame stb's int index would overflow on
// can be asked about without allocating one. The codec must refuse it before reading a byte.
class VastFrameStore final : public IFrameStoreAccess {
 public:
  Result<FrameStoreBudget> Budget() override { return Ok(FrameStoreBudget{}); }
  Result<FrameRef> Allocate(int32_t, int32_t, PixelFormat) override {
    return Err<FrameRef>(StatusCode::Unsupported, "Vast", "no");
  }
  Result<std::span<uint8_t>> Pin(const FrameRef&) override {
    // Never dereferenced: what is under test is that nothing reads it.
    return Ok(std::span<uint8_t>(reinterpret_cast<uint8_t*>(&byte_), size_t{1} << 33));
  }
  Status Release(const FrameRef&) override { return Status::Ok(); }
  Result<Residency> ResidencyOf(const FrameRef&) override { return Ok(Residency::HeapEncoded); }
  Status Demote(const FrameRef&, Residency) override { return Status::Ok(); }
  Status Adopt(const FrameRef&) override { return Status::Ok(); }
  Status Forget(const FrameRef&) override { return Status::Ok(); }
  Status Clear() override { return Status::Ok(); }
  Result<uint64_t> TierGeneration() override { return Ok(uint64_t{1}); }
  Result<uint64_t> ContentHash(const FrameRef&) override { return Ok(uint64_t{0}); }

 private:
  uint8_t byte_ = 0;
};

TEST(StbImageCodecLimits, AFrameTooLargeForTheWritersIndexIsRefused) {
  VastFrameStore vast;
  StbImageCodecAccess codec(vast);
  FrameRef frame;
  frame.id = FrameId{1};
  frame.buffer = BufferId{1};
  frame.format = PixelFormat::RGBA8;
  frame.width = 65535;
  frame.height = 8193;
  frame.stride = 65535 * 4;
  EncodeSpec spec;
  EXPECT_EQ(codec.Encode(frame, spec).status.code, StatusCode::InvalidArgument);
}

// Read and put back: the frame is in the tier it was in, unpinned, whether the encode worked or
// not.
TEST_F(StbImageCodec, TheFrameIsLeftWhereItWas) {
  const FrameRef frame = Picture(16, 8);
  ASSERT_TRUE(store.Demote(frame, Residency::Spilled).ok());
  ASSERT_TRUE(codec.Encode(frame, Jpeg()).ok());
  EXPECT_EQ(store.ResidencyOf(frame).value, Residency::Spilled);
  EncodeSpec flat = Jpeg();
  flat.attachGPanoXmp = true;
  flat.fullPanoWidth = 8;
  flat.fullPanoHeight = 4;
  ASSERT_FALSE(codec.Encode(frame, flat).ok());
  EXPECT_EQ(store.ResidencyOf(frame).value, Residency::Spilled);
  // Refused by the writer, after the frame was lent: put back all the same.
  FrameRef taller = frame;
  taller.height = 4096;
  ASSERT_EQ(codec.Encode(taller, Jpeg()).status.code, StatusCode::InvalidArgument);
  EXPECT_EQ(store.ResidencyOf(frame).value, Residency::Spilled);
  const FrameRef resident = Picture(16, 8);
  ASSERT_TRUE(codec.Encode(resident, Jpeg()).ok());
  EXPECT_EQ(store.ResidencyOf(resident).value, Residency::HeapEncoded) << "and no pin is left";
  FrameRef tallerResident = resident;
  tallerResident.height = 4096;
  ASSERT_EQ(codec.Encode(tallerResident, Jpeg()).status.code, StatusCode::InvalidArgument);
  EXPECT_EQ(store.ResidencyOf(resident).value, Residency::HeapEncoded)
      << "nor by a refusal after the pin";
}

// A store that will not let the frame go, or put it back, is said: an encode that left a pin or a
// faulted-in frame behind and answered success would leak it for the session.
TEST_F(StbImageCodec, AStoreThatWillNotTakeTheFrameBackIsSaid) {
  RecordingFrameStoreAccess recording(store);
  StbImageCodecAccess through(recording);
  const FrameRef frame = Picture(16, 8);
  recording.RefuseReleases(true);
  EXPECT_EQ(through.Encode(frame, Jpeg()).status.code, StatusCode::Internal);
  recording.RefuseReleases(false);
  ASSERT_TRUE(store.Release(frame).ok()) << "the refused release's pin, given back by hand";
  ASSERT_TRUE(store.Demote(frame, Residency::Spilled).ok());
  recording.RefuseDemotions(true);
  EXPECT_EQ(through.Encode(frame, Jpeg()).status.code, StatusCode::Internal);
  ASSERT_FALSE(recording.demotions().empty());
  EXPECT_EQ(recording.demotions().back(), Residency::Spilled);
}

// The byte limit is exactly what an `int` counts, asked without allocating it: a store that will
// not say where a frame is answers for every handle the limits let past, so which answer comes
// back says which side of them the handle landed on, and nothing is read either way.
TEST_F(StbImageCodec, TheByteLimitIsExactlyTheFramesAnIntCounts) {
  RecordingFrameStoreAccess unsure(store);
  unsure.RefuseResidency(true);
  StbImageCodecAccess through(unsure);
  FrameRef frame = Picture(16, 8);
  frame.stride = 0;
  // One pixel under a quarter of `INT_MAX`, with both sides within a JPEG's: as close to the limit
  // as a frame can be and still be let past, to the store.
  frame.width = 9718;
  frame.height = 55245;
  EXPECT_EQ(through.Encode(frame, Jpeg()).status.code, StatusCode::Internal);
  frame.width = 32768;
  frame.height = 16384;
  EXPECT_EQ(through.Encode(frame, Jpeg()).status.code, StatusCode::InvalidArgument)
      << "two gigabytes is refused";
}

// Put back is a demotion only for the tiers a pin faults in from: a spilled frame and one on the
// GPU are demoted to where they were, and a resident one, or one its caller already holds pinned,
// is asked for nothing, since the release alone puts it back.
TEST_F(StbImageCodec, OnlyAFrameFaultedInIsDemotedBack) {
  RecordingFrameStoreAccess recording(store);
  StbImageCodecAccess through(recording);
  const FrameRef frame = Picture(16, 8);
  recording.ClaimResidency(Residency::GpuTexture);
  // This store has no GPU tier, so its refusal of the demotion is the answer; what was asked is
  // the point.
  EXPECT_EQ(through.Encode(frame, Jpeg()).status.code, StatusCode::Unsupported);
  EXPECT_EQ(recording.demotions(), std::vector<Residency>{Residency::GpuTexture});
  recording.ClaimResidency(Residency::HeapEncoded);
  ASSERT_TRUE(through.Encode(frame, Jpeg()).ok());
  EXPECT_EQ(recording.demotions().size(), 1u) << "a resident frame is not demoted";
  // Held by its caller across the encode: still theirs afterwards, and theirs to release.
  ASSERT_TRUE(store.Pin(frame).ok());
  ASSERT_TRUE(codec.Encode(frame, Jpeg()).ok());
  EXPECT_EQ(store.ResidencyOf(frame).value, Residency::HeapPinned);
  ASSERT_TRUE(store.Release(frame).ok());
  EXPECT_EQ(store.ResidencyOf(frame).value, Residency::HeapEncoded);
}

}  // namespace
}  // namespace sphanorama
