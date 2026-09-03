#pragma once

#include <cstdint>

#include "sphanorama/types.h"

namespace sphanorama {

// Zero for planar and encoded formats, which have no single bytes-per-pixel. Callers sizing a
// buffer want FrameByteSize; this exists for stride arithmetic on interleaved formats.
int32_t BytesPerPixel(PixelFormat format);

// Bytes needed to hold one frame, or 0 when the dimensions are nonsense or the size is a
// property of the data rather than the geometry (encoded formats).
int64_t FrameByteSize(int32_t width, int32_t height, PixelFormat format);

}  // namespace sphanorama
