#include "support/fake_image_codec_access.h"

#include <algorithm>

namespace sphanorama {
namespace {
constexpr const char* kComponent = "FakeImageCodecAccess";
// A four-byte header, then the pixels. Enough structure to prove a round trip without pulling in
// an image library the test does not care about.
constexpr uint8_t kMagic[4] = {'S', 'P', 'H', '0'};
}  // namespace

FakeImageCodecAccess::FakeImageCodecAccess(std::shared_ptr<IFrameStoreAccess> store)
    : store_(store ? std::move(store) : std::make_shared<MemoryFrameStoreAccess>(1 << 22)) {}

Result<std::vector<uint8_t>> FakeImageCodecAccess::Encode(const FrameRef& frame,
                                                          const EncodeSpec& spec) {
  auto pinned = store_->Pin(frame);
  if (!pinned.ok()) return pinned.status;

  std::vector<uint8_t> out(std::begin(kMagic), std::end(kMagic));
  out.insert(out.end(), pinned.value.begin(), pinned.value.end());
  if (auto released = store_->Release(frame); !released.ok()) return released;

  last_encode_carried_gpano_ = spec.attachGPanoXmp;
  last_format_ = spec.format;
  return Ok(std::move(out));
}

Result<FrameRef> FakeImageCodecAccess::Decode(std::span<const uint8_t> bytes) {
  if (bytes.size() < sizeof(kMagic) ||
      !std::equal(std::begin(kMagic), std::end(kMagic), bytes.begin())) {
    return Err<FrameRef>(StatusCode::CodecFailure, kComponent, "not a recognised payload");
  }
  const auto payload = bytes.subspan(sizeof(kMagic));
  if (payload.empty()) return Err<FrameRef>(StatusCode::CodecFailure, kComponent, "empty payload");
  // Checked before the division, not after: floor()ing the pixel count and then copying the whole
  // payload into it writes past the end of the allocation for any length that is not a whole
  // number of RGBA pixels.
  if (payload.size() % 4 != 0) {
    return Err<FrameRef>(StatusCode::CodecFailure, kComponent,
                         "payload is not a whole number of RGBA pixels");
  }
  const auto pixels = static_cast<int32_t>(payload.size() / 4);

  auto frame = store_->Allocate(pixels, 1, PixelFormat::RGBA8);
  if (!frame.ok()) return frame;
  auto pinned = store_->Pin(frame.value);
  if (!pinned.ok()) return pinned.status;
  std::copy(payload.begin(), payload.end(), pinned.value.begin());
  if (auto released = store_->Release(frame.value); !released.ok()) return released;
  return frame;
}

}  // namespace sphanorama
