// V16 — how a frame the core is holding is made small enough to look at.
//
// The output is an image, and nobody can write down the right value of a resampled pixel in
// advance. So these are invariants: a reduction of a flat frame is flat and the same colour, a
// reduction keeps the shape it was given, structure that was on the left is still on the left,
// and a channel that was red comes back red. The one thing asserted as a number is the size,
// because the size is the whole reason this exists.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "engines/frame_preview_engine/box_frame_preview_engine.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"

namespace sphanorama {
namespace {

class FramePreviewEngine : public ::testing::Test {
 protected:
  MemoryFrameStoreAccess store{1 << 24};
  BoxFramePreviewEngine engine{store};

  /** A frame whose pixel at (x, y) is whatever `paint` says, as {r, g, b}. */
  template <typename Paint>
  FrameRef Frame(int32_t width, int32_t height, PixelFormat format, Paint paint) {
    auto allocated = store.Allocate(width, height, format);
    EXPECT_TRUE(allocated.ok()) << allocated.status.detail;
    auto pinned = store.Pin(allocated.value);
    EXPECT_TRUE(pinned.ok()) << pinned.status.detail;
    const int32_t channels = format == PixelFormat::Gray8 ? 1 : 4;
    for (int32_t y = 0; y < height; ++y) {
      for (int32_t x = 0; x < width; ++x) {
        const auto rgb = paint(x, y);
        const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(width) +
                           static_cast<size_t>(x)) * static_cast<size_t>(channels);
        if (format == PixelFormat::Gray8) {
          pinned.value[at] = rgb[0];
        } else if (format == PixelFormat::BGRA8) {
          pinned.value[at] = rgb[2];
          pinned.value[at + 1] = rgb[1];
          pinned.value[at + 2] = rgb[0];
          pinned.value[at + 3] = 255;
        } else {
          pinned.value[at] = rgb[0];
          pinned.value[at + 1] = rgb[1];
          pinned.value[at + 2] = rgb[2];
          pinned.value[at + 3] = 255;
        }
      }
    }
    EXPECT_TRUE(store.Release(allocated.value).ok());
    return allocated.value;
  }

  using Rgb = std::array<uint8_t, 3>;

  FrameRef Flat(int32_t width, int32_t height, Rgb colour,
                PixelFormat format = PixelFormat::RGBA8) {
    return Frame(width, height, format, [colour](int32_t, int32_t) { return colour; });
  }

  /** Dark on the left half, bright on the right. Structure a reduction has to keep. */
  FrameRef SplitLeftRight(int32_t width, int32_t height) {
    return Frame(width, height, PixelFormat::RGBA8, [width](int32_t x, int32_t) -> Rgb {
      const uint8_t value = x < width / 2 ? 0 : 255;
      return {value, value, value};
    });
  }

  static Rgb PixelAt(const FramePreview& preview, int32_t x, int32_t y) {
    const size_t at = (static_cast<size_t>(y) * static_cast<size_t>(preview.width) +
                       static_cast<size_t>(x)) * 4;
    return {preview.pixels[at], preview.pixels[at + 1], preview.pixels[at + 2]};
  }
};

TEST_F(FramePreviewEngine, ReducesToTheLongEdgeAskedFor) {
  // The number this whole component exists to produce. 1280x960 at a long edge of 128 is a tenth
  // in each direction, which is 48 KB against the frame's 4.9 MB.
  const FrameRef frame = Flat(1280, 960, {10, 20, 30});

  auto reduced = engine.Reduce(frame, 128);
  ASSERT_TRUE(reduced.ok()) << reduced.status.detail;
  EXPECT_EQ(reduced.value.width, 128);
  EXPECT_EQ(reduced.value.height, 96);
  EXPECT_EQ(reduced.value.format, PixelFormat::RGBA8);
  EXPECT_EQ(reduced.value.frame.value, frame.id.value);
  // Tightly packed RGBA, which is what makes it an ImageData on the other side with nothing in
  // between: four bytes a pixel and no stride of its own.
  EXPECT_EQ(reduced.value.pixels.size(), static_cast<size_t>(128 * 96 * 4));
}

TEST_F(FramePreviewEngine, KeepsTheShapeItWasGiven) {
  // A preview drawn at the wrong aspect ratio is a review client showing a scene that is not the
  // one that was captured, which is worse than showing nothing.
  const FrameRef portrait = Flat(480, 640, {128, 128, 128});

  auto reduced = engine.Reduce(portrait, 64);
  ASSERT_TRUE(reduced.ok()) << reduced.status.detail;
  EXPECT_LE(std::max(reduced.value.width, reduced.value.height), 64);
  const double source = 480.0 / 640.0;
  const double result = static_cast<double>(reduced.value.width) /
                        static_cast<double>(reduced.value.height);
  EXPECT_NEAR(result, source, 0.02);
}

TEST_F(FramePreviewEngine, AFrameAlreadyInsideTheBoundComesBackAtItsOwnSize) {
  // Reducing is what this does. Enlarging would be inventing pixels, which is a different
  // operation with different failure modes, and a strip that upscaled a 32-pixel frame to 128
  // would be claiming detail the capture never had.
  const FrameRef small = Flat(32, 24, {200, 100, 50});

  auto reduced = engine.Reduce(small, 128);
  ASSERT_TRUE(reduced.ok()) << reduced.status.detail;
  EXPECT_EQ(reduced.value.width, 32);
  EXPECT_EQ(reduced.value.height, 24);
  const Rgb pixel = PixelAt(reduced.value, 5, 5);
  EXPECT_EQ(pixel[0], 200);
  EXPECT_EQ(pixel[1], 100);
  EXPECT_EQ(pixel[2], 50);
}

TEST_F(FramePreviewEngine, AFlatFrameReducesToTheSameFlatColour) {
  // The invariant that says the averaging is an averaging: the mean of a constant is that
  // constant, in every channel, at every output pixel. A reduction that returned zeros, or that
  // swapped a channel, or that read past a row, all fail here.
  const FrameRef flat = Flat(640, 480, {30, 90, 210});

  auto reduced = engine.Reduce(flat, 64);
  ASSERT_TRUE(reduced.ok()) << reduced.status.detail;
  for (int32_t y = 0; y < reduced.value.height; ++y) {
    for (int32_t x = 0; x < reduced.value.width; ++x) {
      const Rgb pixel = PixelAt(reduced.value, x, y);
      ASSERT_EQ(pixel[0], 30) << "at " << x << "," << y;
      ASSERT_EQ(pixel[1], 90) << "at " << x << "," << y;
      ASSERT_EQ(pixel[2], 210) << "at " << x << "," << y;
    }
  }
  // Opaque, because a canvas draws the alpha it is given and a preview full of zero alpha is an
  // invisible one that every other assertion here would still pass.
  EXPECT_EQ(reduced.value.pixels[3], 255);
}

TEST_F(FramePreviewEngine, StructureSurvivesTheReduction) {
  // The point of showing the frame rather than its numbers: what is on the left of the capture is
  // on the left of the preview. Averaging into blocks cannot move it, and a reduction that
  // sampled the wrong rows or transposed the image would put it somewhere else.
  const FrameRef split = SplitLeftRight(640, 480);

  auto reduced = engine.Reduce(split, 64);
  ASSERT_TRUE(reduced.ok()) << reduced.status.detail;
  const int32_t middle = reduced.value.height / 2;
  EXPECT_LT(PixelAt(reduced.value, 2, middle)[0], 32);
  EXPECT_GT(PixelAt(reduced.value, reduced.value.width - 3, middle)[0], 223);
}

TEST_F(FramePreviewEngine, ABgraFrameComesBackWithItsChannelsInOrder) {
  // Red and blue are the two channels a byte-order mistake swaps, and a swap is invisible in a
  // grey test frame. Within one capture every frame comes from one camera in one format, so this
  // would not show up until OfferFrame put an imported frame in a cell — which is exactly the
  // path a review client reads back.
  const FrameRef bgra = Flat(64, 64, {240, 10, 5}, PixelFormat::BGRA8);

  auto reduced = engine.Reduce(bgra, 32);
  ASSERT_TRUE(reduced.ok()) << reduced.status.detail;
  const Rgb pixel = PixelAt(reduced.value, 4, 4);
  EXPECT_EQ(pixel[0], 240);
  EXPECT_EQ(pixel[1], 10);
  EXPECT_EQ(pixel[2], 5);
}

TEST_F(FramePreviewEngine, AGreyFrameComesBackGrey) {
  // Gray8 has one channel and a canvas has four. Replicating it is the only reading that shows
  // the frame; taking it as the red channel alone would tint every imported grey frame crimson.
  const FrameRef grey = Flat(64, 64, {180, 180, 180}, PixelFormat::Gray8);

  auto reduced = engine.Reduce(grey, 32);
  ASSERT_TRUE(reduced.ok()) << reduced.status.detail;
  const Rgb pixel = PixelAt(reduced.value, 4, 4);
  EXPECT_EQ(pixel[0], 180);
  EXPECT_EQ(pixel[1], 180);
  EXPECT_EQ(pixel[2], 180);
}

TEST_F(FramePreviewEngine, ALongEdgeOverTheCapIsRefused) {
  // The cap is the budget (ADR 0038). Clamping instead of refusing would hand back something
  // other than what was asked for and let a caller believe it had the frame.
  const FrameRef frame = Flat(1280, 960, {128, 128, 128});

  auto reduced = engine.Reduce(frame, kFramePreviewMaxEdge + 1);
  EXPECT_EQ(reduced.status.code, StatusCode::InvalidArgument);
}

TEST_F(FramePreviewEngine, ALongEdgeOfNothingIsRefused) {
  const FrameRef frame = Flat(1280, 960, {128, 128, 128});

  EXPECT_EQ(engine.Reduce(frame, 0).status.code, StatusCode::InvalidArgument);
  EXPECT_EQ(engine.Reduce(frame, -8).status.code, StatusCode::InvalidArgument);
}

TEST_F(FramePreviewEngine, AFormatWithNoReadablePixelsSaysSoRatherThanGuessing) {
  // An encoded frame needs a decoder this engine is not handed. Returning grey, or the bytes of
  // the JPEG interpreted as pixels, would be a preview of nothing that still looked like an
  // answer.
  FrameRef encoded = Flat(64, 64, {128, 128, 128});
  encoded.format = PixelFormat::EncodedJpeg;

  EXPECT_EQ(engine.Reduce(encoded, 32).status.code, StatusCode::Unsupported);
}

TEST_F(FramePreviewEngine, AFrameTheStoreDoesNotHaveIsNotFound) {
  // A replace-retake forgets a cell's frames, and a review client can be holding a candidate list
  // taken before that happened. The answer has to be the store's, not a crash.
  FrameRef missing;
  missing.id = FrameId{9999};
  missing.format = PixelFormat::RGBA8;
  missing.width = 64;
  missing.height = 64;

  EXPECT_EQ(engine.Reduce(missing, 32).status.code, StatusCode::NotFound);
}

TEST_F(FramePreviewEngine, TheFrameIsReleasedOnEveryPathOut) {
  // Pin guarantees its mapping until Release. An engine that kept one would pin every candidate a
  // user looked at for the rest of the session — and the store's own refusal to demote a pinned
  // frame would then defeat the residency restore above it, so the leak would be two leaks.
  const FrameRef good = Flat(640, 480, {128, 128, 128});
  ASSERT_TRUE(engine.Reduce(good, 64).ok());
  auto residency = store.ResidencyOf(good);
  ASSERT_TRUE(residency.ok());
  EXPECT_NE(residency.value, Residency::HeapPinned);

  // And on the failing path that runs *after* the pin, which is the one that gets written
  // without a release. A refusal made before pinning would pass this test having proved nothing.
  FrameRef lying = Flat(640, 480, {128, 128, 128});
  lying.width = 4000;
  lying.stride = 4000 * 4;
  EXPECT_EQ(engine.Reduce(lying, 64).status.code, StatusCode::FailedPrecondition);
  auto after = store.ResidencyOf(lying);
  ASSERT_TRUE(after.ok());
  EXPECT_NE(after.value, Residency::HeapPinned);
}

TEST_F(FramePreviewEngine, AFrameShorterThanItClaimsIsRefusedRatherThanReadPast) {
  // A FrameRef is a plain value the caller passes in, and the store's entry is the only thing
  // that knows how many bytes are really there. Trusting the handle's dimensions over the span
  // reads off the end of the allocation — which is a crash on a good day and somebody else's
  // pixels on a bad one.
  FrameRef lying = Flat(64, 64, {128, 128, 128});
  lying.height = 4096;

  auto reduced = engine.Reduce(lying, 32);
  EXPECT_EQ(reduced.status.code, StatusCode::FailedPrecondition);
}

TEST_F(FramePreviewEngine, AFrameThinnerThanOneBlockIsNotReadPastItsEdge) {
  // The block is sized from the *longest* edge, so a very wide, very short frame gets a block
  // taller than the frame is. `rows` is then forced up to 1 — a preview of no rows is not an
  // image — and the sampling window went on being `block` tall regardless, reading rows that do
  // not exist. Off the end of the allocation, which is a crash on a good day and somebody else's
  // pixels on a bad one, and the same failure the handle-versus-bytes check above guards the
  // other approach to.
  //
  // Not exotic: `OfferFrame` takes a frame the caller allocated, in whatever shape it liked.
  //
  // The colour is what makes this assert rather than merely execute. A flat frame reduces to its
  // own colour, so anything sampled from beyond the last row drags the average off it — and the
  // sanitizer build fails outright, which is the sharper of the two signals.
  const Rgb colour{200, 100, 50};
  for (const auto& shape : {std::pair<int32_t, int32_t>{64, 1}, {1, 64}, {96, 3}, {3, 96}}) {
    const FrameRef frame = Flat(shape.first, shape.second, colour);
    auto preview = engine.Reduce(frame, 8);
    ASSERT_TRUE(preview.ok()) << shape.first << "x" << shape.second << ": "
                              << preview.status.detail;
    ASSERT_FALSE(preview.value.pixels.empty());
    for (size_t at = 0; at + 3 < preview.value.pixels.size(); at += 4) {
      EXPECT_EQ(preview.value.pixels[at], colour[0]) << shape.first << "x" << shape.second;
      EXPECT_EQ(preview.value.pixels[at + 1], colour[1]) << shape.first << "x" << shape.second;
      EXPECT_EQ(preview.value.pixels[at + 2], colour[2]) << shape.first << "x" << shape.second;
    }
  }
}

}  // namespace
}  // namespace sphanorama
