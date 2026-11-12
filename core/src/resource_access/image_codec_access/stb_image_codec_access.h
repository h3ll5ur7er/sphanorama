#pragma once

#include <span>
#include <vector>

#include "sphanorama/resource_access/frame_store_access.h"
#include "sphanorama/resource_access/image_codec_access.h"

namespace sphanorama {

// A codec inside the core, over stb's JPEG writer, and the same one in every build (ADR 0087). The
// browser's encoders answer asynchronously and a port answers within the call, so the page has no
// encoder to lend; one compiled into the core needs none.
//
// It reads pixels through the frame store, which the contract's `FrameRef` names, and leaves each
// frame unpinned in the tier it found it in. It writes JPEG and nothing else, and decodes nothing:
// nothing in the core reads an image file yet, and a decoder no caller exercises is a parser
// exposed for nothing.
class StbImageCodecAccess final : public IImageCodecAccess {
 public:
  explicit StbImageCodecAccess(IFrameStoreAccess& frames) : frames_(frames) {}

  // `Unsupported`, until something imports a file.
  Result<FrameRef> Decode(std::span<const uint8_t> bytes) override;

  // An `RGBA8` frame as a JPEG at the spec's quality, one to a hundred, its alpha dropped. With
  // `attachGPanoXmp` it is tagged as part of an equirectangular sphere: the sphere the spec names,
  // as wide as the frame, with the frame a band centred on its horizon, or with none named the
  // frame as the whole of one. `InvalidArgument` for a quality out of range, a handle that names
  // another format or no pixels, a side past a JPEG's 65535, a frame of two gigabytes or more, a
  // handle claiming more bytes than the store holds for it, and a frame that is not part of the
  // sphere asked for. The store says nothing of a frame's format, so only the handle's claim and
  // the byte count are checked: a handle naming the wrong format over as many bytes is written as
  // what it names. `Unsupported` for AVIF and PNG; the store's own status where it would not lend
  // the frame or take it back.
  Result<std::vector<uint8_t>> Encode(const FrameRef& frame, const EncodeSpec& spec) override;

 private:
  IFrameStoreAccess& frames_;
};

}  // namespace sphanorama
