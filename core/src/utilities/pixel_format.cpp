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
    case PixelFormat::I420:
      // 12 bits per pixel: a full luma plane plus a quarter-resolution chroma pair.
      return pixels + (pixels / 2);
    case PixelFormat::EncodedJpeg:
    case PixelFormat::Unknown:
      return 0;
    default:
      return pixels * BytesPerPixel(format);
  }
}

}  // namespace sphanorama
