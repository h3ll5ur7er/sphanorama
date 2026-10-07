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
// only while they stay well short of half a turn apart, which they do within 15 degrees of every
// cell up to 80 degrees off the horizon (measured: 0.24 degrees of roll for 0.05 of turn, at worst).
constexpr double kCellReferenceWithinDeg = 5.0;
constexpr double kLevelReferenceBeyondDeg = 15.0;

double Wrap(double degrees) {
  if (degrees > 180.0) return degrees - 360.0;
  if (degrees <= -180.0) return degrees + 360.0;
  return degrees;
}

}  // namespace

double GuidanceRollDeg(const Quat& current, const Quat& target) {
  const double againstCell = RollBetween(current, target) * kRadToDeg;
  const double separationDeg =
      AngleBetweenDirections(Direction(current), Direction(target)) * kRadToDeg;
  if (!(separationDeg > kCellReferenceWithinDeg)) return againstCell;
  const double fromLevel = RollFromLevel(current) * kRadToDeg;
  if (separationDeg >= kLevelReferenceBeyondDeg) return fromLevel;
  const double towardLevel = (separationDeg - kCellReferenceWithinDeg) /
                             (kLevelReferenceBeyondDeg - kCellReferenceWithinDeg);
  // The difference wrapped first, so the blend goes the short way round: rolled near a half-turn
  // the two can read -179 and 176, five degrees apart as angles and 355 as numbers.
  return Wrap(againstCell + towardLevel * Wrap(fromLevel - againstCell));
}

}  // namespace sphanorama
