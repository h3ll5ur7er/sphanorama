// The coverage planner (V4).
//
// The expected output of a tessellation is not knowable in advance, but its invariants are, and
// they are the ones a sphere silently fails on (docs/00 §0.2). The load-bearing one is
// completeness: every direction on the sphere has to fall inside some cell's field of view, or
// the capture ends with a hole nothing reported.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <vector>

#include "engines/coverage_planner_engine/rings_coverage_planner_engine.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRadToDeg = 180.0 / kPi;

CapturePlanSpec Spec(double h = 66.0, double v = 50.0, double overlap = 0.30) {
  CapturePlanSpec spec;
  spec.horizontalFovDeg = h;
  spec.verticalFovDeg = v;
  spec.overlapTarget = overlap;
  spec.acceptanceConeDeg = 5.0;
  spec.coverPoles = true;
  return spec;
}

// The elevation a cell aims at, in degrees. The plan stores orientations, not angles, and
// reading one back is how a test says "near the pole" without trusting the planner's own maths.
double ElevationOf(const Quat& orientation) {
  const Vec3 direction = Direction(orientation);
  return std::asin(std::clamp(direction.y, -1.0, 1.0)) * kRadToDeg;
}

CapturePlan Plan(const CapturePlanSpec& spec) {
  RingsCoveragePlannerEngine planner;
  auto plan = planner.Plan(spec, Intrinsics{});
  EXPECT_TRUE(plan.ok()) << plan.status.detail;
  return plan.value;
}

/** Points spread evenly over the sphere — a Fibonacci lattice, so no axis is favoured. */
std::vector<Vec3> SphereSamples(int count) {
  std::vector<Vec3> points;
  points.reserve(static_cast<size_t>(count));
  const double golden = kPi * (3.0 - std::sqrt(5.0));
  for (int i = 0; i < count; ++i) {
    const double y = 1.0 - 2.0 * (static_cast<double>(i) + 0.5) / count;
    const double radius = std::sqrt(std::max(0.0, 1.0 - y * y));
    const double theta = golden * i;
    points.push_back(Vec3{std::cos(theta) * radius, y, std::sin(theta) * radius});
  }
  return points;
}

/** True when `direction` falls inside the rectangular field of view centred on `cell`. */
bool InsideFieldOfView(const Quat& cell, const Vec3& direction, double hFovDeg, double vFovDeg) {
  // Bring the direction into the cell's own frame, where forward is -Z.
  const Vec3 local = Rotate(Conjugate(cell), direction);
  const double yaw = std::atan2(local.x, -local.z) * kRadToDeg;
  const double pitch = std::asin(std::clamp(local.y, -1.0, 1.0)) * kRadToDeg;
  return std::abs(yaw) <= hFovDeg * 0.5 + 1e-9 && std::abs(pitch) <= vFovDeg * 0.5 + 1e-9;
}

bool CoveredByAnyCell(const CapturePlan& plan, const Vec3& direction) {
  for (const auto& node : plan.nodes) {
    if (InsideFieldOfView(node.targetOrientation, direction, plan.spec.horizontalFovDeg,
                          plan.spec.verticalFovDeg)) {
      return true;
    }
  }
  return false;
}

// ------------------------------------------------------------------------------ completeness

TEST(CoveragePlanner, EveryDirectionOnTheSphereIsInsideSomeCell) {
  // The invariant the whole plan exists for. A gap here is a hole in the finished sphere that
  // nothing would report until someone looked at the seam.
  const CapturePlan plan = Plan(Spec());
  int uncovered = 0;
  for (const Vec3& direction : SphereSamples(4000)) {
    if (!CoveredByAnyCell(plan, direction)) ++uncovered;
  }
  EXPECT_EQ(uncovered, 0) << "of 4000 sampled directions, with " << plan.nodes.size() << " cells";
}

TEST(CoveragePlanner, StaysCompleteForANarrowLens) {
  // A long lens is where a spacing bug hides: more cells, each covering less.
  const CapturePlan plan = Plan(Spec(35.0, 26.0));
  for (const Vec3& direction : SphereSamples(4000)) {
    ASSERT_TRUE(CoveredByAnyCell(plan, direction));
  }
}

TEST(CoveragePlanner, StaysCompleteForAWideLens) {
  const CapturePlan plan = Plan(Spec(100.0, 80.0));
  for (const Vec3& direction : SphereSamples(4000)) {
    ASSERT_TRUE(CoveredByAnyCell(plan, direction));
  }
}

TEST(CoveragePlanner, CoversBothPoles) {
  // Straight up and straight down are the directions a ring layout most easily leaves out.
  const CapturePlan plan = Plan(Spec());
  EXPECT_TRUE(CoveredByAnyCell(plan, Vec3{0, 1, 0}));
  EXPECT_TRUE(CoveredByAnyCell(plan, Vec3{0, -1, 0}));
}

// ----------------------------------------------------------------------------------- overlap

TEST(CoveragePlanner, NeighbouringCellsOverlap) {
  // Registration needs shared features between adjacent frames; cells that merely abut give it
  // nothing to match on.
  const CapturePlan plan = Plan(Spec());
  for (const auto& node : plan.nodes) {
    double nearest = 360.0;
    for (const auto& other : plan.nodes) {
      if (other.id.value == node.id.value) continue;
      nearest = std::min(nearest,
                         AngleBetween(node.targetOrientation, other.targetOrientation) * kRadToDeg);
    }
    EXPECT_LT(nearest, plan.spec.horizontalFovDeg)
        << "a cell whose nearest neighbour is a whole field of view away shares no features";
  }
}

// -------------------------------------------------------------------------------- cell counts

TEST(CoveragePlanner, AWiderLensNeedsFewerCells) {
  EXPECT_LT(Plan(Spec(100.0, 80.0)).nodes.size(), Plan(Spec(50.0, 40.0)).nodes.size());
}

TEST(CoveragePlanner, MoreOverlapNeedsMoreCells) {
  EXPECT_GT(Plan(Spec(66.0, 50.0, 0.50)).nodes.size(), Plan(Spec(66.0, 50.0, 0.20)).nodes.size());
}

TEST(CoveragePlanner, ATypicalPhoneLensGivesAWorkableNumberOfCells) {
  // Not a precise expectation — a bound. Ten cells would not cover a sphere; three hundred is a
  // capture nobody finishes.
  const size_t cells = Plan(Spec()).nodes.size();
  EXPECT_GE(cells, 20u);
  EXPECT_LE(cells, 150u);
}

TEST(CoveragePlanner, RingsAreNarrowerNearThePoles) {
  // Azimuth spacing has to widen as the rings shrink, or the poles get hundreds of cells nobody
  // needs to shoot.
  const CapturePlan plan = Plan(Spec());
  std::map<int32_t, int> perRing;
  for (const auto& node : plan.nodes) ++perRing[node.ringIndex];
  ASSERT_GE(perRing.size(), 3u);

  const int equatorial = std::max_element(
      perRing.begin(), perRing.end(),
      [](const auto& a, const auto& b) { return a.second < b.second; })->second;
  EXPECT_GT(equatorial, perRing.begin()->second);
  EXPECT_GT(equatorial, perRing.rbegin()->second);
}

// ------------------------------------------------------------------------------ plan identity

TEST(CoveragePlanner, CellIdsAreUnique) {
  const CapturePlan plan = Plan(Spec());
  std::set<uint64_t> ids;
  for (const auto& node : plan.nodes) ids.insert(node.id.value);
  EXPECT_EQ(ids.size(), plan.nodes.size());
}

TEST(CoveragePlanner, NoTwoCellsPointAtTheSameDirection) {
  const CapturePlan plan = Plan(Spec());
  for (size_t i = 0; i < plan.nodes.size(); ++i) {
    for (size_t j = i + 1; j < plan.nodes.size(); ++j) {
      EXPECT_GT(AngleBetween(plan.nodes[i].targetOrientation, plan.nodes[j].targetOrientation),
                1e-6) << "cells " << i << " and " << j;
    }
  }
}

TEST(CoveragePlanner, PlanningTwiceGivesTheSamePlan) {
  // A build is keyed on the plan; an unstable one would invalidate cached stages for nothing.
  const CapturePlan a = Plan(Spec());
  const CapturePlan b = Plan(Spec());
  ASSERT_EQ(a.nodes.size(), b.nodes.size());
  for (size_t i = 0; i < a.nodes.size(); ++i) {
    EXPECT_EQ(a.nodes[i].id.value, b.nodes[i].id.value);
    EXPECT_NEAR(AngleBetween(a.nodes[i].targetOrientation, b.nodes[i].targetOrientation), 0.0,
                1e-12);
  }
}

TEST(CoveragePlanner, EveryCellCarriesTheAcceptanceConeItWasAskedFor) {
  for (const auto& node : Plan(Spec()).nodes) EXPECT_DOUBLE_EQ(node.acceptanceConeDeg, 5.0);
}

// -------------------------------------------------------------------------------- validation

TEST(CoveragePlanner, RefusesALensItWasNotToldAbout) {
  RingsCoveragePlannerEngine planner;
  CapturePlanSpec spec = Spec();
  spec.horizontalFovDeg = 0.0;
  EXPECT_EQ(planner.Plan(spec, Intrinsics{}).status.code, StatusCode::InvalidArgument);
}

TEST(CoveragePlanner, RefusesNonsenseOverlap) {
  RingsCoveragePlannerEngine planner;
  CapturePlanSpec spec = Spec();
  spec.overlapTarget = 1.0;   // a step of zero degrees: infinitely many cells
  EXPECT_EQ(planner.Plan(spec, Intrinsics{}).status.code, StatusCode::InvalidArgument);
  spec.overlapTarget = -0.5;
  EXPECT_EQ(planner.Plan(spec, Intrinsics{}).status.code, StatusCode::InvalidArgument);
}

TEST(CoveragePlanner, RefusesAnAcceptanceConeOfZero) {
  RingsCoveragePlannerEngine planner;
  CapturePlanSpec spec = Spec();
  spec.acceptanceConeDeg = 0.0;
  EXPECT_EQ(planner.Plan(spec, Intrinsics{}).status.code, StatusCode::InvalidArgument);
}

// ----------------------------------------------------------------------------------- locate

TEST(CoveragePlanner, LocateFindsTheCellTheUserIsAimingAt) {
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());
  for (const auto& node : plan.nodes) {
    auto guidance = planner.Locate(node.targetOrientation, plan);
    ASSERT_TRUE(guidance.ok());
    EXPECT_EQ(guidance.value.targetNode.value, node.id.value);
    EXPECT_NEAR(guidance.value.angularErrorDeg, 0.0, 1e-9);
    EXPECT_EQ(guidance.value.action, GuidanceAction::HoldStill);
  }
}

TEST(CoveragePlanner, LocateAsksTheUserToKeepLookingWhenTheyAreOff) {
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());
  const Quat aim = Multiply(plan.nodes.front().targetOrientation, FromAzimuthElevation(20.0, 0.0));
  auto guidance = planner.Locate(aim, plan);
  ASSERT_TRUE(guidance.ok());
  EXPECT_EQ(guidance.value.action, GuidanceAction::Seek);
  EXPECT_GT(guidance.value.angularErrorDeg, plan.spec.acceptanceConeDeg);
}

TEST(CoveragePlanner, NoDirectionIsFurtherFromACellThanAFieldOfView) {
  // Guidance has to have something to point at wherever the user happens to be looking.
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());
  for (const Vec3& direction : SphereSamples(500)) {
    const double azimuth = std::atan2(direction.x, -direction.z) * kRadToDeg;
    const double elevation = std::asin(std::clamp(direction.y, -1.0, 1.0)) * kRadToDeg;
    auto guidance = planner.Locate(FromAzimuthElevation(azimuth, elevation), plan);
    ASSERT_TRUE(guidance.ok());
    EXPECT_LT(guidance.value.angularErrorDeg, plan.spec.horizontalFovDeg);
  }
}

TEST(CoveragePlanner, LocateRefusesAnEmptyPlan) {
  RingsCoveragePlannerEngine planner;
  EXPECT_EQ(planner.Locate(Quat{}, CapturePlan{}).status.code, StatusCode::FailedPrecondition);
}

// --------------------------------------------------------------------------------- evaluate

TEST(CoveragePlanner, EveryCellIsAHoleBeforeAnythingIsShot) {
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());
  auto state = planner.Evaluate(plan, {});
  ASSERT_TRUE(state.ok());
  EXPECT_EQ(state.value.nodesSatisfied, 0);
  EXPECT_EQ(state.value.holes.size(), plan.nodes.size());
  EXPECT_DOUBLE_EQ(state.value.coveredSolidAngleFraction, 0.0);
}

TEST(CoveragePlanner, ShootingACellSatisfiesItAndOnlyIt) {
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());
  Candidate candidate;
  candidate.node = plan.nodes.front().id;
  const std::vector<Candidate> candidates{candidate};

  auto state = planner.Evaluate(plan, candidates);
  ASSERT_TRUE(state.ok());
  EXPECT_EQ(state.value.nodesSatisfied, 1);
  EXPECT_EQ(state.value.holes.size(), plan.nodes.size() - 1);
}

TEST(CoveragePlanner, RetakesAreSuggestedForHolesAndGhostsAlike) {
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());

  CoverageState state;
  state.holes.push_back(plan.nodes[1].id);
  GhostReport ghosts;
  GhostRegion region;
  region.node = plan.nodes[2].id;
  ghosts.regions.push_back(region);

  auto suggestions = planner.SuggestRetakes(plan, state, ghosts);
  ASSERT_TRUE(suggestions.ok());
  ASSERT_EQ(suggestions.value.size(), 2u);
  EXPECT_EQ(suggestions.value[0].value, plan.nodes[1].id.value);
  EXPECT_EQ(suggestions.value[1].value, plan.nodes[2].id.value);
}

TEST(CoveragePlanner, ACellThatIsBothAHoleAndGhostedIsSuggestedOnce) {
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());

  CoverageState state;
  state.holes.push_back(plan.nodes[1].id);
  GhostReport ghosts;
  GhostRegion region;
  region.node = plan.nodes[1].id;
  ghosts.regions.push_back(region);

  auto suggestions = planner.SuggestRetakes(plan, state, ghosts);
  ASSERT_TRUE(suggestions.ok());
  EXPECT_EQ(suggestions.value.size(), 1u);
}


TEST(RingsCoveragePlanner, CoverPolesFalseLeavesTheCapsOut) {
  // The spec option exists because a sphere shot indoors is mostly ceiling and floor nobody
  // wants, and asking for a band should not silently return a full sphere: the plan would echo
  // coverPoles=false while handing back the cells it says it left out.
  RingsCoveragePlannerEngine engine;
  CapturePlanSpec spec = Spec();
  spec.coverPoles = false;

  auto banded = engine.Plan(spec, Intrinsics{});
  ASSERT_TRUE(banded.ok()) << banded.status.detail;

  spec.coverPoles = true;
  auto full = engine.Plan(spec, Intrinsics{});
  ASSERT_TRUE(full.ok());

  EXPECT_LT(banded.value.nodes.size(), full.value.nodes.size());
  for (const auto& node : banded.value.nodes) {
    // Nothing within a lens-height of either pole.
    const double elevation = ElevationOf(node.targetOrientation);
    EXPECT_LT(std::abs(elevation), 90.0 - spec.verticalFovDeg / 2.0)
        << "a cell at " << elevation << " degrees is a pole cap";
  }
}

TEST(RingsCoveragePlanner, ABandStillClosesAroundTheHorizon) {
  // Dropping the caps must not thin the ring the user actually starts on.
  RingsCoveragePlannerEngine engine;
  CapturePlanSpec spec = Spec();
  spec.coverPoles = false;

  auto plan = engine.Plan(spec, Intrinsics{});
  ASSERT_TRUE(plan.ok());
  int onHorizon = 0;
  for (const auto& node : plan.value.nodes) {
    if (std::abs(ElevationOf(node.targetOrientation)) < 1e-6) ++onHorizon;
  }
  EXPECT_GE(onHorizon, 8);
}


TEST(RingsCoveragePlanner, RefusesAFieldOfViewThatIsNotAFiniteAngle) {
  // The spec arrives from JavaScript across the facade, where every number is a double and NaN
  // and Infinity are ordinary values. Both survive the "> 0" check and reach ceil(90 / step),
  // whose conversion to int is undefined behaviour for anything outside int's range.
  RingsCoveragePlannerEngine engine;
  for (const double bad : {std::numeric_limits<double>::quiet_NaN(),
                           std::numeric_limits<double>::infinity()}) {
    CapturePlanSpec spec = Spec();
    spec.verticalFovDeg = bad;
    EXPECT_EQ(engine.Plan(spec, Intrinsics{}).status.code, StatusCode::InvalidArgument);
    spec = Spec();
    spec.horizontalFovDeg = bad;
    EXPECT_EQ(engine.Plan(spec, Intrinsics{}).status.code, StatusCode::InvalidArgument);
  }
}

TEST(RingsCoveragePlanner, RefusesALensSoNarrowThePlanCouldNotBeHeld) {
  // A vanishingly small field of view is arithmetically valid and produces a ring count that
  // overflows int on the way to a plan nobody could capture. Refusing is the only useful answer.
  RingsCoveragePlannerEngine engine;
  CapturePlanSpec spec = Spec();
  spec.verticalFovDeg = 1e-300;
  EXPECT_EQ(engine.Plan(spec, Intrinsics{}).status.code, StatusCode::InvalidArgument);

  spec = Spec();
  spec.horizontalFovDeg = 1e-300;
  EXPECT_EQ(engine.Plan(spec, Intrinsics{}).status.code, StatusCode::InvalidArgument);
}


TEST(RingsCoveragePlanner, RefusesAStrategyItDoesNotImplement) {
  // The plan echoes the spec back, so silently substituting rings for a geodesic tessellation
  // would have the plan claim a layout it does not have — and a caller reasoning about cell
  // spacing from the strategy it asked for would be reasoning about the wrong sphere.
  RingsCoveragePlannerEngine engine;
  for (const auto strategy : {TessellationStrategy::Geodesic, TessellationStrategy::Adaptive}) {
    CapturePlanSpec spec = Spec();
    spec.strategy = strategy;
    auto plan = engine.Plan(spec, Intrinsics{});
    EXPECT_EQ(plan.status.code, StatusCode::Unsupported);
  }
}

TEST(CoveragePlanner, RollDoesNotCountAsBeingOffTarget) {
  // Angular error is how far the camera is from pointing at the cell. Comparing whole attitudes
  // folds rotation about the optical axis into that number, so a phone aimed exactly at a cell
  // but held at an angle reads as far off target and the reticle never closes. Roll has its own
  // field precisely because it is a different correction for the user to make.
  RingsCoveragePlannerEngine engine;
  const CapturePlan plan = Plan(Spec());

  const Quat aimed = plan.nodes.front().targetOrientation;
  auto straight = engine.Locate(aimed, plan);
  ASSERT_TRUE(straight.ok());

  // The same direction, rotated 30 degrees about the axis the camera looks along.
  const Vec3 axis = Direction(aimed);
  const Quat rolled = Multiply(FromAxisAngle(axis, 30.0 / kRadToDeg), aimed);
  auto tilted = engine.Locate(rolled, plan);
  ASSERT_TRUE(tilted.ok());

  EXPECT_EQ(tilted.value.targetNode.value, straight.value.targetNode.value);
  EXPECT_NEAR(tilted.value.angularErrorDeg, straight.value.angularErrorDeg, 1e-6);
  EXPECT_NEAR(std::abs(tilted.value.rollErrorDeg), 30.0, 1e-6);
}

TEST(CoveragePlanner, ReportsNoRollWhenTheCameraIsUpright) {
  RingsCoveragePlannerEngine engine;
  const CapturePlan plan = Plan(Spec());
  auto guidance = engine.Locate(plan.nodes.front().targetOrientation, plan);
  ASSERT_TRUE(guidance.ok());
  EXPECT_NEAR(guidance.value.rollErrorDeg, 0.0, 1e-9);
}

TEST(CoveragePlanner, PicksTheCellTheCameraActuallyPointsAt) {
  // With roll excluded, "nearest" has to mean nearest in direction. A rolled phone must still
  // select the cell in front of it rather than a neighbour that happens to match its attitude.
  RingsCoveragePlannerEngine engine;
  const CapturePlan plan = Plan(Spec());
  for (const auto& node : plan.nodes) {
    const Vec3 axis = Direction(node.targetOrientation);
    const Quat rolled = Multiply(FromAxisAngle(axis, 45.0 / kRadToDeg), node.targetOrientation);
    auto guidance = engine.Locate(rolled, plan);
    ASSERT_TRUE(guidance.ok());
    EXPECT_EQ(guidance.value.targetNode.value, node.id.value);
    EXPECT_NEAR(guidance.value.angularErrorDeg, 0.0, 1e-6);
  }
}

}  // namespace
}  // namespace sphanorama
