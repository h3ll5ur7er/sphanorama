#include "resource_access/image_codec_access/stb_image_codec_access.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>

#include "utilities/gpano_xmp.h"

#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace sphanorama {
namespace {

constexpr const char* kComponent = "StbImageCodecAccess";
constexpr int64_t kChannels = 4;
// A JPEG writes each side in sixteen bits, and stb writes them without asking.
constexpr int32_t kLongestSide = 65535;
// A frame's bytes have to fit an `int`: stb indexes them with one, its largest four short of the
// frame's size, and on wasm32 the packed copy of a padded frame is a vector no larger.
constexpr int64_t kMostBytes = std::numeric_limits<int>::max();

void Append(void* context, void* data, int size) {
  auto* out = static_cast<std::vector<uint8_t>*>(context);
  const auto* bytes = static_cast<const uint8_t*>(data);
  out->insert(out->end(), bytes, bytes + size);
}

// Where the frame sits in the sphere: the one named, with the frame a band centred on its horizon,
// or none named and the frame the whole of one.
std::optional<GPanoArea> AreaOf(const FrameRef& frame, const EncodeSpec& spec) {
  GPanoArea area;
  area.croppedWidth = frame.width;
  area.croppedHeight = frame.height;
  if (spec.fullPanoWidth == 0 && spec.fullPanoHeight == 0) {
    area.fullWidth = frame.width;
    area.fullHeight = frame.height;
    return area;
  }
  // A band goes all the way round, so a frame narrower than the sphere is not one: placing it
  // would mean choosing an azimuth nobody gave.
  if (spec.fullPanoWidth != frame.width) return std::nullopt;
  area.fullWidth = spec.fullPanoWidth;
  area.fullHeight = spec.fullPanoHeight;
  area.croppedTop = static_cast<int32_t>((int64_t{spec.fullPanoHeight} - frame.height) / 2);
  return area;
}

// The frame's pixels as a JPEG. The handle is the caller's account of the frame and the span is the
// store's, so the one is checked against the other before a byte is read — by division, because the
// product is what overflows.
Result<std::vector<uint8_t>> Write(const FrameRef& frame, std::span<const uint8_t> pinned,
                                   int32_t quality) {
  const int64_t rowBytes = int64_t{frame.width} * kChannels;
  const int64_t stride = frame.stride > 0 ? frame.stride : rowBytes;
  const int64_t held = static_cast<int64_t>(pinned.size());
  if (stride < rowBytes) {
    return Err<std::vector<uint8_t>>(StatusCode::InvalidArgument, kComponent,
                                     "this frame's stride is narrower than one row of it");
  }
  const int64_t rows = int64_t{frame.height} - 1;
  if (rowBytes > held || (rows > 0 && stride > (held - rowBytes) / rows)) {
    return Err<std::vector<uint8_t>>(StatusCode::InvalidArgument, kComponent,
                                     "this frame claims more pixels than the store is holding");
  }
  // The writer reads packed rows, so a padded frame is packed first.
  std::vector<uint8_t> packed;
  const uint8_t* rgba = pinned.data();
  if (stride != rowBytes) {
    packed.resize(static_cast<size_t>(rowBytes) * static_cast<size_t>(frame.height));
    for (int64_t y = 0; y < frame.height; ++y) {
      const uint8_t* from = pinned.data() + y * stride;
      std::copy(from, from + rowBytes, packed.begin() + static_cast<std::ptrdiff_t>(y * rowBytes));
    }
    rgba = packed.data();
  }
  std::vector<uint8_t> jpeg;
  if (stbi_write_jpg_to_func(&Append, &jpeg, frame.width, frame.height,
                             static_cast<int>(kChannels), rgba, quality) == 0) {
    return Err<std::vector<uint8_t>>(StatusCode::CodecFailure, kComponent,
                                     "the JPEG writer refused this frame");
  }
  return Ok(std::move(jpeg));
}

}  // namespace

Result<FrameRef> StbImageCodecAccess::Decode(std::span<const uint8_t>) {
  return Err<FrameRef>(StatusCode::Unsupported, kComponent, "nothing reads an image file yet");
}

Result<std::vector<uint8_t>> StbImageCodecAccess::Encode(const FrameRef& frame,
                                                         const EncodeSpec& spec) {
  if (spec.format != EncodeFormat::Jpeg) {
    return Err<std::vector<uint8_t>>(StatusCode::Unsupported, kComponent,
                                     "this codec writes JPEG and nothing else");
  }
  if (spec.quality < 1 || spec.quality > 100) {
    return Err<std::vector<uint8_t>>(StatusCode::InvalidArgument, kComponent,
                                     "a JPEG's quality runs from one to a hundred");
  }
  if (frame.format != PixelFormat::RGBA8 || frame.width <= 0 || frame.height <= 0) {
    return Err<std::vector<uint8_t>>(StatusCode::InvalidArgument, kComponent,
                                     "only an RGBA8 frame with pixels is written");
  }
  // Past either, the writer would answer a smaller picture, or index past its own int, under the
  // name of this one. By division, since the product is the thing that overflows.
  if (frame.width > kLongestSide || frame.height > kLongestSide
      || frame.height > kMostBytes / kChannels / frame.width) {
    return Err<std::vector<uint8_t>>(StatusCode::InvalidArgument, kComponent,
                                     "this frame is larger than a JPEG can be written at");
  }
  // Asked before a byte is read, so a refused tag costs no encode.
  std::optional<std::string> packet;
  if (spec.attachGPanoXmp) {
    const std::optional<GPanoArea> area = AreaOf(frame, spec);
    if (area) packet = GPanoPacket(*area);
    if (!packet) {
      return Err<std::vector<uint8_t>>(
          StatusCode::InvalidArgument, kComponent,
          "this frame is not part of the sphere it would be tagged as");
    }
  }

  SPH_TRY(const Residency before, frames_.ResidencyOf(frame));
  SPH_TRY(const std::span<uint8_t> pinned, frames_.Pin(frame));
  Result<std::vector<uint8_t>> written = Write(frame, pinned, spec.quality);
  // Released and put back on every path out, and a store that refuses either is said: a pin kept
  // would hold the frame on the heap for the life of the session, and a spilled frame left faulted
  // in would be paid for twice.
  if (Status released = frames_.Release(frame); !released.ok()) return released;
  if (before == Residency::Spilled || before == Residency::GpuTexture) {
    if (Status put = frames_.Demote(frame, before); !put.ok()) return put;
  }
  if (!written.ok() || !packet) return written;
  return WithXmpPacket(written.value, *packet);
}

}  // namespace sphanorama
