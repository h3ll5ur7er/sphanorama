#pragma once

#include <cstdint>
#include <optional>

#include "sphanorama/types.h"
#include "utilities/camera_model.h"

namespace sphanorama {

// The direction a point of an equirectangular panorama looks in, in world space.
//
// `tools/synth_dataset.py`'s convention, which is the only other place it is written: longitude 0
// is -Z and increases toward +X, latitude +90 is +Y at row 0, and an integer coordinate is a pixel
// *edge*, so the centre of pixel (i, j) is (i + 0.5, j + 0.5). A compositor and the renderer its
// accuracy is measured against must agree on this without either being derived from the other, so
// each is pinned to the same hand-worked decimals (ADR 0050).
//
// Unit when answered. Nothing for a panorama with no area, a coordinate that is not finite, or a row
// outside [0, height]: past the top is off the sphere, not the pole. A column outside [0, width]
// wraps, because longitude does.
std::optional<Vec3> EquirectDirection(const Pixel& at, int32_t width, int32_t height);

}  // namespace sphanorama
