#include "utilities/pixel_format.h"

namespace sphanorama {

int32_t BytesPerPixel(PixelFormat format) {
  switch (format) {
    case PixelFormat::RGBA8:
    case PixelFormat::BGRA8:
      return 4;
    case PixelFormat::Gray8:
      return 1;
    case PixelFormat::NV12:
    case PixelFormat::I420:
    case PixelFormat::EncodedJpeg:
    case PixelFormat::Unknown:
      return 0;
  }
  return 0;
}

int64_t FrameByteSize(int32_t width, int32_t height, PixelFormat format) {
  if (width <= 0 || height <= 0) return 0;

  const int64_t pixels = static_cast<int64_t>(width) * height;
  switch (format) {
    case PixelFormat::NV12:
    case PixelFormat::I420: {
      // A full luma plane plus a chroma plane at half resolution in each direction. "12 bits per
      // pixel" is the even-dimension shorthand for this and undercounts every odd one: a 3x3
      // frame carries a 2x2 chroma plane, not a 1.5x1.5 one, so each dimension rounds up on its
      // own before they are multiplied.
      const int64_t chromaWidth = (static_cast<int64_t>(width) + 1) / 2;
      const int64_t chromaHeight = (static_cast<int64_t>(height) + 1) / 2;
      return pixels + chromaWidth * chromaHeight * 2;
    }
    case PixelFormat::EncodedJpeg:
    case PixelFormat::Unknown:
      return 0;
    default:
      return pixels * BytesPerPixel(format);
  }
}

}  // namespace sphanorama
