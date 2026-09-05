#include "engines/frame_preview_engine/box_frame_preview_engine.h"

#include <algorithm>
#include <array>

#include "utilities/pixel_format.h"

namespace sphanorama {
namespace {

constexpr const char* kComponent = "BoxFramePreviewEngine";

/** Colour at (x, y) as {r, g, b}, in the frame's own channel order. */
std::array<uint32_t, 3> ColourAt(std::span<const uint8_t> bytes, PixelFormat format,
                                 int64_t stride, int64_t x, int64_t y) {
  // int64 arithmetic, then one conversion to the span's index type. A 4K frame's last row is past
  // 2^31 bytes in at four bytes a pixel, so the multiply has to be wide even though the result
  // always fits a size_t on the platforms this runs on.
  const int64_t row = y * stride;
  if (format == PixelFormat::Gray8) {
    // One channel replicated into three. Taking it as red alone would tint every imported grey
    // frame crimson, which looks like a colour cast in the capture rather than a bug here.
    const uint32_t value = bytes[static_cast<size_t>(row + x)];
    return {value, value, value};
  }
  const size_t at = static_cast<size_t>(row + x * 4);
  const bool bgra = format == PixelFormat::BGRA8;
  return {bytes[at + (bgra ? 2u : 0u)], bytes[at + 1], bytes[at + (bgra ? 0u : 2u)]};
}

bool HasReadableColour(PixelFormat format) {
  switch (format) {
    case PixelFormat::RGBA8:
    case PixelFormat::BGRA8:
    case PixelFormat::Gray8:
      return true;
    default:
      return false;
  }
}

}  // namespace

Result<FramePreview> BoxFramePreviewEngine::Reduce(const FrameRef& frame, int32_t maxEdge) {
  if (maxEdge <= 0 || maxEdge > kFramePreviewMaxEdge) {
    return Err<FramePreview>(StatusCode::InvalidArgument, kComponent,
                             "a preview's long edge has to be between 1 and " +
                                 std::to_string(kFramePreviewMaxEdge) + " pixels");
  }
  if (!HasReadableColour(frame.format)) {
    return Err<FramePreview>(StatusCode::Unsupported, kComponent,
                             "this frame's format has no colour this engine can read; a planar "
                             "or encoded frame needs a decoder it is not handed");
  }
  if (frame.width <= 0 || frame.height <= 0) {
    return Err<FramePreview>(StatusCode::InvalidArgument, kComponent, "a frame with no pixels");
  }

  auto pinned = frames_.Pin(frame);
  if (!pinned.ok()) return pinned.status;

  // Undone on every path out, including the failures below: Pin promises the mapping until
  // Release, and a pin left behind would hold every candidate a user looked at in the heap for
  // the rest of the session — and the store refuses to demote a pinned frame, so the residency
  // restore above this would silently stop working too.
  struct Unpin {
    IFrameStoreAccess& frames;
    const FrameRef& frame;
    ~Unpin() { (void)frames.Release(frame); }
  } unpin{frames_, frame};

  const int64_t derived =
      static_cast<int64_t>(frame.width) * std::max(BytesPerPixel(frame.format), 1);
  const int64_t stride = frame.stride > 0 ? frame.stride : derived;

  // A stride is bytes per row, so it cannot be less than a row. Checked before the span, because
  // it is what makes the span check mean anything: `stride * height` is small when the stride is
  // understated, so a handle claiming a wide row in a narrow stride satisfies a requirement of a
  // few dozen bytes and then reads across the full width anyway. Sixty-four pixels claimed in a
  // four-byte stride passes a 32-byte requirement and reads 283 bytes into a 256-byte frame.
  if (stride < derived) {
    return Err<FramePreview>(StatusCode::FailedPrecondition, kComponent,
                             "this frame's stride is narrower than one row of its own width");
  }

  // The handle's dimensions are checked against the bytes that actually arrived. A `FrameRef` is
  // a plain value the caller passes in and the store's entry is the only thing that knows how
  // large the allocation really is; trusting the handle reads off the end of it, which is a crash
  // on a good day and somebody else's pixels on a bad one.
  //
  // `stride * height` rather than the last row's end, deliberately. With the stride sane it is
  // the stricter of the two — it asks for the trailing padding of the final row, which every
  // allocation this store makes actually has — and on a bounds check the stricter side is the
  // safe one.
  const int64_t needed = stride * frame.height;
  if (needed <= 0 || static_cast<size_t>(needed) > pinned.value.size()) {
    return Err<FramePreview>(StatusCode::FailedPrecondition, kComponent,
                             "this frame holds fewer bytes than its handle describes");
  }

  // Whole blocks averaged into one output pixel. Integer blocks rather than interpolation for the
  // same reason the sharpness measure downscales this way: a resampler is a second thing to get
  // wrong, and the truncation costs at most `block - 1` pixels off the right and bottom edges.
  const int64_t longest = std::max(frame.width, frame.height);
  const int64_t block = std::max<int64_t>(1, (longest + maxEdge - 1) / maxEdge);
  const int64_t cols = std::max<int64_t>(1, frame.width / block);
  const int64_t rows = std::max<int64_t>(1, frame.height / block);

  // The window each output pixel averages, per axis, and it is not always the block.
  //
  // A block is sized from the *longest* edge, so a wide, short frame gets one taller than the
  // frame is — and the row count above is then forced up to 1, because a preview of no rows is
  // not an image. Sampling `block` rows of a frame that has two reads off the end of the
  // allocation. Clamping per axis is what makes the forced count honest: the window is the
  // dimension itself when the dimension is smaller.
  //
  // It changes nothing in the ordinary case. `cols` reaches 1 without being forced exactly when
  // `frame.width >= block`, so the clamp only ever bites on the shape that would have read out of
  // bounds.
  const int64_t windowX = std::min<int64_t>(block, frame.width);
  const int64_t windowY = std::min<int64_t>(block, frame.height);

  FramePreview preview;
  preview.frame = frame.id;
  preview.format = PixelFormat::RGBA8;
  preview.width = static_cast<int32_t>(cols);
  preview.height = static_cast<int32_t>(rows);
  preview.pixels.assign(static_cast<size_t>(cols * rows * 4), 0u);

  // Sixty-four bits, for the same reason the offsets above are. A block is the whole frame when
  // `maxEdge` is 1, so `area` is the pixel count and each channel's sum is that times 255: a
  // frame past about 4100 on a side overflows a `uint32_t` and the average comes out darker than
  // anything in the picture. No frame here is that large yet — the grabber caps its long edge at
  // 1280 — and no test asserts it, because the frame that would demonstrate it is 67 MB, which is
  // half a phone's whole frame-store ceiling (ADR 0023). Widening it costs nothing and removes
  // the trap from the arithmetic rather than from a comment.
  const uint64_t area = static_cast<uint64_t>(windowX) * static_cast<uint64_t>(windowY);
  for (int64_t row = 0; row < rows; ++row) {
    for (int64_t col = 0; col < cols; ++col) {
      std::array<uint64_t, 3> sum{0, 0, 0};
      for (int64_t dy = 0; dy < windowY; ++dy) {
        for (int64_t dx = 0; dx < windowX; ++dx) {
          const auto rgb = ColourAt(pinned.value, frame.format, stride, col * block + dx,
                                    row * block + dy);
          sum[0] += rgb[0];
          sum[1] += rgb[1];
          sum[2] += rgb[2];
        }
      }
      const size_t at = static_cast<size_t>(row * cols + col) * 4;
      preview.pixels[at] = static_cast<uint8_t>(sum[0] / area);
      preview.pixels[at + 1] = static_cast<uint8_t>(sum[1] / area);
      preview.pixels[at + 2] = static_cast<uint8_t>(sum[2] / area);
      // Opaque. Whatever alpha the source carried, a preview is something to look at rather than
      // something to composite, and a canvas handed a zero alpha draws nothing at all.
      preview.pixels[at + 3] = 255u;
    }
  }
  return Ok(std::move(preview));
}

}  // namespace sphanorama
