#pragma once
#include <span>
#include <vector>
#include "sphanorama/types.h"

namespace sphanorama {

// V13 — how images are decoded, encoded and tagged. Owns the XMP GPano block, which is what
// makes an exported file open as a sphere rather than a wide photo.
//
// Not mirrored across the boundary: the browser's encoders answer asynchronously and a port answers
// within the call, so the browser never implements this, and one codec inside the core serves every
// build (ADR 0087).
class IImageCodecAccess {
 public:
  virtual ~IImageCodecAccess() = default;

  virtual Result<FrameRef> Decode(std::span<const uint8_t> bytes) = 0;
  virtual Result<std::vector<uint8_t>> Encode(const FrameRef& frame, const EncodeSpec& spec) = 0;
};

}  // namespace sphanorama
