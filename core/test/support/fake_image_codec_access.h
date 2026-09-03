#pragma once

#include <memory>
#include <vector>

#include "sphanorama/resource_access/frame_store_access.h"
#include "sphanorama/resource_access/image_codec_access.h"
#include "support/fake_frame_store_access.h"

namespace sphanorama {

// A codec that round-trips through a trivial container rather than a real image format. Manager
// tests care that bytes survive encode/decode and that the GPano request is honoured, not that
// the result is a valid JPEG — decoding a real one would be testing libjpeg.
class FakeImageCodecAccess final : public IImageCodecAccess {
 public:
  explicit FakeImageCodecAccess(std::shared_ptr<IFrameStoreAccess> store = nullptr);

  Result<FrameRef> Decode(std::span<const uint8_t> bytes) override;
  Result<std::vector<uint8_t>> Encode(const FrameRef& frame, const EncodeSpec& spec) override;

  bool LastEncodeCarriedGPano() const { return last_encode_carried_gpano_; }
  EncodeFormat LastFormat() const { return last_format_; }

 private:
  std::shared_ptr<IFrameStoreAccess> store_;
  bool last_encode_carried_gpano_ = false;
  EncodeFormat last_format_ = EncodeFormat::Jpeg;
};

}  // namespace sphanorama
