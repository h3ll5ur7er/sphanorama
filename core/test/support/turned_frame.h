#pragma once
#include <cstring>
#include <numbers>

#include "sphanorama/resource_access/frame_store_access.h"
#include "utilities/pixel_format.h"
#include "utilities/quaternion.h"

namespace sphanorama::test {

// A frame as a phone turned a quarter clockwise about the way it looks would have handed it back:
// the old left edge is the new top, so the new pixel (i, j) is the old pixel (j, height - 1 - i).
// A packed format only. The answer is a new allocation in `store` and belongs to the caller.
inline Result<FrameRef> TurnedClockwise(IFrameStoreAccess& store, const FrameRef& frame) {
  const int32_t bytes = BytesPerPixel(frame.format);
  if (bytes <= 0) {
    return Err<FrameRef>(StatusCode::InvalidArgument, "turned_frame", "not a packed format");
  }
  Result<FrameRef> turned = store.Allocate(frame.height, frame.width, frame.format);
  if (!turned.ok()) return turned;
  const Result<std::span<uint8_t>> from = store.Pin(frame);
  if (!from.ok()) {
    (void)store.Forget(turned.value);
    return from.status;
  }
  const Result<std::span<uint8_t>> to = store.Pin(turned.value);
  if (!to.ok()) {
    (void)store.Release(frame);
    (void)store.Forget(turned.value);
    return to.status;
  }
  for (int32_t j = 0; j < turned.value.height; ++j) {
    for (int32_t i = 0; i < turned.value.width; ++i) {
      const int32_t x = j;
      const int32_t y = frame.height - 1 - i;
      const size_t row = static_cast<size_t>(j) * static_cast<size_t>(turned.value.stride);
      std::memcpy(to.value.data() + row + static_cast<size_t>(i) * static_cast<size_t>(bytes),
                  from.value.data() + static_cast<size_t>(y) * static_cast<size_t>(frame.stride) +
                      static_cast<size_t>(x) * static_cast<size_t>(bytes),
                  static_cast<size_t>(bytes));
    }
  }
  (void)store.Release(turned.value);
  (void)store.Release(frame);
  return turned;
}

// The device-to-world rotation of that turned frame: its camera's right is the old down, which is
// a quarter turn about the camera's forward axis, -Z, composed on the camera's side.
inline Quat RolledClockwise(const Quat& rotation) {
  return Normalize(Multiply(rotation, FromAxisAngle(Vec3{0.0, 0.0, 1.0}, std::numbers::pi / 2.0)));
}

}  // namespace sphanorama::test
