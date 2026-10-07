#include "engines/coverage_planner_engine/guidance_roll.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr double kRadToDeg = 180.0 / std::numbers::pi;

// Where the reference starts to move from the cell and where it has become level, as the angle
// between the phone's view and the cell's. A band, not a point, because the two references can
// differ by tens of degrees at any separation; this one is narrow because the blend is continuous
// only while they stay short of half a turn apart, which within 15 degrees they do for every cell
// at least 15 from a pole.
constexpr double kCellReferenceWithinDeg = 5.0;
constexpr double kLevelReferenceBeyondDeg = 15.0;

// Level fades out toward a pole, where it has no meaning and the roll from it spins faster the
// closer the phone gets: none of it within 5 degrees, all of it beyond 15.
constexpr double kLevelGoneWithinDeg = 5.0;
constexpr double kLevelWholeBeyondDeg = 15.0;

double Wrap(double degrees) {
  if (degrees > 180.0) return degrees - 360.0;
  if (degrees <= -180.0) return degrees + 360.0;
  return degrees;
}

double Ramp(double degrees, double from, double to) {
  return std::clamp((degrees - from) / (to - from), 0.0, 1.0);
}

double FromPoleDeg(const Vec3& view) {
  return 90.0 - std::abs(std::asin(std::clamp(view.y, -1.0, 1.0))) * kRadToDeg;
}

}  // namespace

double GuidanceRollDeg(const Quat& current, const Quat& target) {
  const Vec3 looking = Direction(current);
  const Vec3 aimed = Direction(target);
  const double fromLevel = Ramp(FromPoleDeg(looking), kLevelGoneWithinDeg, kLevelWholeBeyondDeg) *
                           RollFromLevel(current) * kRadToDeg;
  // Around a pole the cell's own reference turns once per circuit and level does not, so no blend
  // of the two is continuous on a circle round it — and within 15 of such a cell that circle is in
  // the band (ADR 0072).
  if (FromPoleDeg(aimed) < kLevelReferenceBeyondDeg) return fromLevel;
  const double againstCell = RollBetween(current, target) * kRadToDeg;
  const double separationDeg = AngleBetweenDirections(looking, aimed) * kRadToDeg;
  const double towardLevel = Ramp(separationDeg, kCellReferenceWithinDeg, kLevelReferenceBeyondDeg);
  // The difference wrapped first, so the blend goes the short way round: rolled near a half-turn
  // the two can read -179 and 176, five degrees apart as angles and 355 as numbers.
  return Wrap(againstCell + towardLevel * Wrap(fromLevel - againstCell));
}

}  // namespace sphanorama
