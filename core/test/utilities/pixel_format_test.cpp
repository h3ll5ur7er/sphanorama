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
