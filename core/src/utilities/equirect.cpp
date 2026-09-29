#include "utilities/equirect.h"

#include <cmath>
#include <numbers>

namespace sphanorama {

std::optional<Vec3> EquirectDirection(const Pixel& at, int32_t width, int32_t height) {
  if (width <= 0 || height <= 0 || !std::isfinite(at.x) || !std::isfinite(at.y)) return std::nullopt;
  if (at.y < 0.0 || at.y > static_cast<double>(height)) return std::nullopt;
  const double longitude = (at.x / width - 0.5) * 2.0 * std::numbers::pi;
  const double latitude = (0.5 - at.y / height) * std::numbers::pi;
  const double across = std::cos(latitude);
  return Vec3{across * std::sin(longitude), std::sin(latitude), -across * std::cos(longitude)};
}

}  // namespace sphanorama
