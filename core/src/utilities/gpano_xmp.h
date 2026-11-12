#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sphanorama/types.h"

namespace sphanorama {

// Where an image sits in the equirectangular sphere it is part of, in pixels of that sphere.
struct GPanoArea {
  int32_t fullWidth = 0, fullHeight = 0;
  int32_t croppedWidth = 0, croppedHeight = 0;
  int32_t croppedLeft = 0, croppedTop = 0;
};

// The XMP packet that tells a viewer an image is part of a photo sphere: Google's GPano properties,
// which Photos, WebXR viewers and most sphere viewers read (ADR 0087). Nothing for an area that is
// not part of a sphere — one with no pixels, a full sphere that is not twice as wide as it is tall,
// or a crop that runs past its edges — since a packet for it would place the image wrongly.
std::optional<std::string> GPanoPacket(const GPanoArea& area);

// `jpeg` with `packet` in an XMP APP1 segment, after the start of image and the JFIF segment if
// there is one, where a reader looks for it before the image data. `InvalidArgument` for bytes that
// do not start as a JPEG does, a JFIF segment that runs past the end, and a packet longer than one
// segment's sixteen-bit length can carry.
Result<std::vector<uint8_t>> WithXmpPacket(std::span<const uint8_t> jpeg, std::string_view packet);

}  // namespace sphanorama
