#pragma once

#include <memory>
#include <vector>

#include "sphanorama/resource_access/frame_store_access.h"
#include "sphanorama/resource_access/image_codec_access.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"

namespace sphanorama {

// A codec that round-trips through a trivial container rather than a real image format. Manager
// tests care that bytes survive encode/decode and that the GPano request is honoured, not that
// the result is a valid JPEG — decoding a real one would be testing libjpeg.
class FakeImageCodecAccess final : public IImageCodecAccess {
 public:
  explicit FakeImageCodecAccess(std::shared_ptr<IFrameStoreAccess> store = nullptr);

  Result<FrameRef> Decode(std::span<const uint8_t> bytes) override;
  Result<std::vector<uint8_t>> Encode(const FrameRef& frame, const EncodeSpec& spec) override;

  // The spec of the last encode written, so a caller is seen to pass on what it was asked for.
  const EncodeSpec& LastSpec() const { return last_spec_; }
  bool LastEncodeCarriedGPano() const { return last_spec_.attachGPanoXmp; }
  EncodeFormat LastFormat() const { return last_spec_.format; }
  // Every call, refused or not, so "before anything is encoded" can be asserted.
  int EncodeCalls() const { return encode_calls_; }
  // Every encode refused with `code`, so a caller's answer to a codec that cannot write is
  // reachable; `Ok` restores it.
  void RefuseEncodes(StatusCode code) { refusal_ = code; }

 private:
  std::shared_ptr<IFrameStoreAccess> store_;
  EncodeSpec last_spec_;
  int encode_calls_ = 0;
  StatusCode refusal_ = StatusCode::Ok;
};

}  // namespace sphanorama
