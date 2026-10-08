#include "engines/coverage_planner_engine/guidance_roll.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr double kRadToDeg = 180.0 / std::numbers::pi;

// The roll from level spins faster the closer the phone looks to a pole — at a fortieth of a degree
// from one, a twentieth of a degree of aim can turn it half a turn — so it fades out between these
// two angles from the pole.
constexpr double kLevelGoneWithinDeg = 5.0;
constexpr double kLevelWholeBeyondDeg = 15.0;

}  // namespace

double GuidanceRollDeg(const Quat& current) {
  const Vec3 looking = Direction(current);
  const double fromPoleDeg =
      90.0 - std::abs(std::asin(std::clamp(looking.y, -1.0, 1.0))) * kRadToDeg;
  const double weight = std::clamp((fromPoleDeg - kLevelGoneWithinDeg) /
                                       (kLevelWholeBeyondDeg - kLevelGoneWithinDeg),
                                   0.0, 1.0);
  return weight * RollFromLevel(current) * kRadToDeg;
}

}  // namespace sphanorama
