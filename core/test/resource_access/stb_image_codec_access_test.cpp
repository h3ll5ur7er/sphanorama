// The codec an export encodes through (ADR 0087): a frame from the store in, a JPEG out, tagged as
// a photo sphere when asked. Checked by decoding the file it wrote, with a decoder that is not the
// encoder, rather than by trusting the call.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
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
  EXPECT_FALSE(Contains(jpeg.value, "GPano")) << "tagged as a sphere without being asked";
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
  EXPECT_TRUE(Contains(jpeg.value, "GPano:ProjectionType=\"equirectangular\""));
  EXPECT_TRUE(Contains(jpeg.value, "GPano:FullPanoWidthPixels=\"64\""));
  EXPECT_TRUE(Contains(jpeg.value, "GPano:CroppedAreaImageHeightPixels=\"32\""));
  EXPECT_TRUE(Contains(jpeg.value, std::string("http://ns.adobe.com/xap/1.0/") + '\0'));
  EXPECT_LT(MeanError(frame, jpeg.value), 3.0);
}

// A band about the horizon, in a sphere the spec names: placed centred, as a capture with no caps
// is, and refused where it cannot be part of the sphere named.
TEST_F(StbImageCodec, ABandIsPlacedAboutTheHorizonOfTheSphereNamed) {
  const FrameRef frame = Picture(64, 16);
  EncodeSpec spec = Jpeg();
  spec.attachGPanoXmp = true;
  spec.fullPanoWidth = 64;
  spec.fullPanoHeight = 32;
  auto jpeg = codec.Encode(frame, spec);
  ASSERT_TRUE(jpeg.ok()) << jpeg.status.detail;
  EXPECT_TRUE(Contains(jpeg.value, "GPano:CroppedAreaTopPixels=\"8\""));
  EXPECT_TRUE(Contains(jpeg.value, "GPano:CroppedAreaLeftPixels=\"0\""));
  // Of a frame that is not a sphere by itself, with no sphere named: nothing to place it in.
  spec.fullPanoWidth = 0;
  spec.fullPanoHeight = 0;
  EXPECT_EQ(codec.Encode(frame, spec).status.code, StatusCode::InvalidArgument);
  spec.fullPanoWidth = 32;
  spec.fullPanoHeight = 16;
  EXPECT_EQ(codec.Encode(frame, spec).status.code, StatusCode::InvalidArgument)
      << "a sphere smaller than the frame";
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
  const FrameRef resident = Picture(16, 8);
  ASSERT_TRUE(codec.Encode(resident, Jpeg()).ok());
  EXPECT_EQ(store.ResidencyOf(resident).value, Residency::HeapEncoded) << "and no pin is left";
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

}  // namespace
}  // namespace sphanorama
