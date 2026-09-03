// Frame stores, codecs and blend kernels all need to size a buffer from a format. One shared
// answer keeps them from disagreeing by a stride.
#include <gtest/gtest.h>

#include "utilities/pixel_format.h"

namespace sphanorama {
namespace {

TEST(BytesPerPixel, KnowsTheInterleavedFormats) {
  EXPECT_EQ(BytesPerPixel(PixelFormat::RGBA8), 4);
  EXPECT_EQ(BytesPerPixel(PixelFormat::BGRA8), 4);
  EXPECT_EQ(BytesPerPixel(PixelFormat::Gray8), 1);
}

TEST(BytesPerPixel, ReportsZeroForFormatsWithNoFixedPixelSize) {
  // Planar and encoded formats have no single bytes-per-pixel; callers must use FrameByteSize.
  EXPECT_EQ(BytesPerPixel(PixelFormat::NV12), 0);
  EXPECT_EQ(BytesPerPixel(PixelFormat::I420), 0);
  EXPECT_EQ(BytesPerPixel(PixelFormat::EncodedJpeg), 0);
  EXPECT_EQ(BytesPerPixel(PixelFormat::Unknown), 0);
}

TEST(FrameByteSize, MultipliesOutInterleavedFormats) {
  EXPECT_EQ(FrameByteSize(4, 3, PixelFormat::RGBA8), 48);
  EXPECT_EQ(FrameByteSize(4, 3, PixelFormat::Gray8), 12);
}

TEST(FrameByteSize, HandlesPlanarFormatsAtTheirRealSize) {
  // NV12 is 12 bits per pixel: a full-resolution luma plane plus a half-resolution chroma plane.
  // Sizing it as width*height would truncate every frame the camera hands us.
  EXPECT_EQ(FrameByteSize(4, 4, PixelFormat::NV12), 24);
  EXPECT_EQ(FrameByteSize(4, 4, PixelFormat::I420), 24);
}

TEST(FrameByteSize, RoundsEachChromaDimensionUpIndependently) {
  // 1.5 bytes per pixel is only true for even dimensions. NV12 stores one interleaved chroma
  // sample per 2x2 luma block, and a 3x3 image has a 2x2 chroma plane, not a 1.5x1.5 one — so the
  // shortcut allocates 13 bytes for a frame that needs 17 and the last chroma row is written
  // past the end.
  EXPECT_EQ(FrameByteSize(3, 3, PixelFormat::NV12), 9 + 2 * 2 * 2);
  EXPECT_EQ(FrameByteSize(3, 3, PixelFormat::I420), 9 + 2 * 2 * 2);
  EXPECT_EQ(FrameByteSize(1, 1, PixelFormat::NV12), 1 + 1 * 1 * 2);
  // An odd width with an even height rounds only the dimension that needs it.
  EXPECT_EQ(FrameByteSize(5, 4, PixelFormat::NV12), 20 + 3 * 2 * 2);
}

TEST(FrameByteSize, RefusesASizeThatCannotBeRepresented) {
  // width*height fits in int64, but multiplying by four does not: the product wraps to something
  // small or negative, and a store that trusted it would allocate a buffer far shorter than the
  // frame written into it. Signed overflow is undefined behaviour, so this is not merely a wrong
  // number — it is a build the optimiser is entitled to reason about however it likes.
  constexpr int32_t kHuge = 2147483647;
  EXPECT_EQ(FrameByteSize(kHuge, kHuge, PixelFormat::RGBA8), 0);
  // Twelve bits per pixel still fits at these dimensions, so the planar path is not refused —
  // this is a representability boundary, not a blanket ban on large numbers.
  EXPECT_GT(FrameByteSize(kHuge, kHuge, PixelFormat::NV12), 0);
  EXPECT_GT(FrameByteSize(kHuge, 1, PixelFormat::RGBA8), 0);
  // Two bytes per pixel is the widest that still fits a full square frame.
  EXPECT_EQ(FrameByteSize(kHuge, kHuge, PixelFormat::Gray8), int64_t{kHuge} * kHuge);
}

TEST(FrameByteSize, RejectsNonsenseDimensions) {
  EXPECT_EQ(FrameByteSize(0, 8, PixelFormat::RGBA8), 0);
  EXPECT_EQ(FrameByteSize(-4, 8, PixelFormat::RGBA8), 0);
}

TEST(FrameByteSize, HasNoAnswerForEncodedData) {
  // An encoded frame's size is a property of the bytes, not of its dimensions.
  EXPECT_EQ(FrameByteSize(1920, 1080, PixelFormat::EncodedJpeg), 0);
}

}  // namespace
}  // namespace sphanorama
