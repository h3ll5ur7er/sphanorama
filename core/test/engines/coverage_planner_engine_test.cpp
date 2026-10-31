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
#include <numbers>
#include <set>
#include <utility>
#include <vector>

#include "engines/coverage_planner_engine/null_coverage_planner_engine.h"
#include "engines/coverage_planner_engine/rings_coverage_planner_engine.h"
#include "utilities/camera_model.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr double kRadToDeg = 180.0 / std::numbers::pi;

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
  const double golden = std::numbers::pi * (3.0 - std::sqrt(5.0));
  for (int i = 0; i < count; ++i) {
    const double y = 1.0 - 2.0 * (static_cast<double>(i) + 0.5) / count;
    const double radius = std::sqrt(std::max(0.0, 1.0 - y * y));
    const double theta = golden * i;
    points.push_back(Vec3{std::cos(theta) * radius, y, std::sin(theta) * radius});
  }
  return points;
}

/** True when `direction` falls inside the frame a camera at `cell` images. */
bool InsideFieldOfView(const Quat& cell, const Vec3& direction, double hFovDeg, double vFovDeg) {
  // Bring the direction into the cell's own frame, where forward is -Z, and ask the image plane.
  // A frame is a rectangle there, which is narrower at its corners than a box of yaw and pitch
  // angles: this read the box, and so credited each frame with corners no camera images.
  const Vec3 local = Rotate(Conjugate(cell), direction);
  const double depth = -local.z;
  if (!(depth > 0.0)) return false;
  const double halfWidth = std::tan(hFovDeg * 0.5 / kRadToDeg);
  const double halfHeight = std::tan(vFovDeg * 0.5 / kRadToDeg);
  return std::abs(local.x) <= halfWidth * depth + 1e-9 &&
         std::abs(local.y) <= halfHeight * depth + 1e-9;
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

// A lens as the page derives it: `longEdgeDeg` on the long edge of a frame `aspect` long by 1
// short, held upright (`deriveFieldOfView`), or sideways, with 30% overlap and a 4 degree cone
// (`main.ts`).
CapturePlanSpec UprightSpec(double longEdgeDeg, double aspect, bool sideways = false) {
  const double shortEdge =
      2.0 * std::atan(std::tan(longEdgeDeg / 2.0 / kRadToDeg) / aspect) * kRadToDeg;
  CapturePlanSpec spec =
      sideways ? Spec(longEdgeDeg, shortEdge, 0.30) : Spec(shortEdge, longEdgeDeg, 0.30);
  spec.acceptanceConeDeg = 4.0;
  return spec;
}

// The plan the page makes: its assumed 66 degrees on a 4:3 frame.
CapturePlanSpec ThePagesSpec() { return UprightSpec(66.0, 4.0 / 3.0); }

// Every lens a phone's main camera could plausibly report, upright and sideways: long edges from
// 45 to 85 degrees on the three shapes a browser hands back. The plan used to be checked on chosen
// lenses, and each one passed while a lens a tenth of a degree away did not (ADR 0076).
std::vector<CapturePlanSpec> PhoneLenses() {
  std::vector<CapturePlanSpec> lenses;
  for (const double aspect : {4.0 / 3.0, 3.0 / 2.0, 16.0 / 9.0}) {
    for (double longEdge = 45.0; longEdge <= 85.0; longEdge += 2.5) {
      for (const bool sideways : {false, true}) {
        lenses.push_back(UprightSpec(longEdge, aspect, sideways));
      }
    }
  }
  return lenses;
}

// The cone's edge, all the way round, and its centre.
// A third of a step round, because at half a step every third aim fell on one of the planner's own
// sixteen, and a check that asks where the planner asked agrees with it for free there.
std::vector<Quat> AimsInTheCone(double coneDeg) {
  std::vector<Quat> aims{Quat{}};
  for (int step = 0; step < 24; ++step) {
    const double around = 2.0 * std::numbers::pi * (step + 1.0 / 3.0) / 24.0;
    aims.push_back(
        FromAxisAngle(Vec3{std::cos(around), std::sin(around), 0.0}, coneDeg / kRadToDeg));
  }
  return aims;
}

// Directions no single cell covers whichever of `variants` its frame is taken at. For one
// direction this is the worst case exactly: each cell's frame is taken once and independently, so
// a direction is a hole in some capture whenever every cell has a variant that misses it.
//
// A frame at a pole is taken at any heading as well, since nothing there asks for one: guidance
// stops correcting roll within five degrees of straight up or down (ADR 0072). Offset by a third of
// a step, which lands on none of the planner's headings, every fifteen degrees, at any step.
int UncoveredInTheWorstCase(const CapturePlan& plan, const std::vector<Quat>& variants,
                            const std::vector<Vec3>& directions, int headingStepDeg = 10) {
  std::vector<Quat> atAPole;
  for (double heading = headingStepDeg / 3.0; heading < 180.0; heading += headingStepDeg) {
    for (const Quat& variant : variants) {
      atAPole.push_back(Multiply(FromAxisAngle(Vec3{0.0, 0.0, 1.0}, heading / kRadToDeg), variant));
    }
  }
  int uncovered = 0;
  for (const Vec3& direction : directions) {
    const bool safe = std::any_of(plan.nodes.begin(), plan.nodes.end(), [&](const auto& node) {
      const bool pole = std::abs(Direction(node.targetOrientation).y) > 1.0 - 1e-9;
      const std::vector<Quat>& taken = pole ? atAPole : variants;
      return std::all_of(taken.begin(), taken.end(), [&](const Quat& variant) {
        return InsideFieldOfView(Multiply(node.targetOrientation, variant), direction,
                                 plan.spec.horizontalFovDeg, plan.spec.verticalFovDeg);
      });
    });
    if (!safe) ++uncovered;
  }
  return uncovered;
}

int UncoveredInTheWorstCase(const CapturePlan& plan, const std::vector<Quat>& variants,
                            int directions = 5000, int headingStepDeg = 10) {
  return UncoveredInTheWorstCase(plan, variants, SphereSamples(directions), headingStepDeg);
}

TEST(CoveragePlanner, StaysCompleteWhereverInsideItsConeAFrameIsTaken) {
  // A burst is armed anywhere inside a cell's cone, and every frame of it is taken there
  // (ADR 0075), so the plan is only complete if it is complete for frames off the cell's centre.
  // Spaced only by the overlap, it was not: the cone ate the overlap on narrow lenses, the caps
  // beside the poles opened for a pole frame turned to its short edge, and the page's own lens
  // passed by rounding (ADR 0076).
  for (const CapturePlanSpec& lens : PhoneLenses()) {
    const CapturePlan plan = Plan(lens);
    EXPECT_EQ(UncoveredInTheWorstCase(plan, AimsInTheCone(plan.spec.acceptanceConeDeg)), 0)
        << lens.horizontalFovDeg << " by " << lens.verticalFovDeg << ", " << plan.nodes.size()
        << " cells";
  }
}

TEST(CoveragePlanner, StaysCompleteWithNoOverlapAtAll) {
  // An overlap of zero is one the planner accepts, so it is one the plan has to be complete at.
  // Frames that abut leave nothing to register by, which is the caller's trade; holes are not.
  for (CapturePlanSpec lens : PhoneLenses()) {
    lens.overlapTarget = 0.0;
    const CapturePlan plan = Plan(lens);
    EXPECT_EQ(UncoveredInTheWorstCase(plan, AimsInTheCone(plan.spec.acceptanceConeDeg)), 0)
        << lens.horizontalFovDeg << " by " << lens.verticalFovDeg << ", " << plan.nodes.size()
        << " cells";
  }
}

TEST(CoveragePlanner, TheCapsCloseForAPoleFrameAtAnyHeadingWithNoOverlap) {
  // A pole frame is taken at any heading, and with no overlap its short edge is where the cap
  // opens: a review found these lenses, held sideways, leaving directions at 66 degrees open that
  // the sweeps above sample between. Asked here at 20,000 directions and every five degrees of
  // heading.
  for (const auto& [longEdge, aspect] : {std::pair{70.0, 4.0 / 3.0}, std::pair{76.0, 3.0 / 2.0},
                                         std::pair{76.5, 3.0 / 2.0}}) {
    CapturePlanSpec lens = UprightSpec(longEdge, aspect, true);
    lens.overlapTarget = 0.0;
    const CapturePlan plan = Plan(lens);
    EXPECT_EQ(
        UncoveredInTheWorstCase(plan, AimsInTheCone(plan.spec.acceptanceConeDeg), 20000, 5), 0)
        << longEdge << " degrees at " << aspect << ", " << plan.nodes.size() << " cells";
  }
}

TEST(CoveragePlanner, ABandClosesTheHorizonWhereverInItsConeAFrameIsTaken) {
  // A band answers for the elevations its rings aim at. With only the horizon ring left that is the
  // horizon itself, which no sample spread over the sphere lands on, and a check of nothing passed:
  // a review found this band leaving a thirtieth of the horizon open.
  CapturePlanSpec lens = Spec(77.5, 110.0, 0.0);
  lens.acceptanceConeDeg = 4.0;
  lens.coverPoles = false;
  const CapturePlan plan = Plan(lens);
  std::vector<Vec3> horizon;
  for (int tenth = 0; tenth < 3600; ++tenth) {
    const double azimuth = tenth / 10.0 / kRadToDeg;
    horizon.push_back(Vec3{std::sin(azimuth), 0.0, -std::cos(azimuth)});
  }
  EXPECT_EQ(UncoveredInTheWorstCase(plan, AimsInTheCone(lens.acceptanceConeDeg), horizon), 0)
      << plan.nodes.size() << " cells";
}

TEST(CoveragePlanner, ABandIsCompleteBetweenItsOuterRings) {
  // Without the caps a band answers for the elevations its rings aim at, and only those, so that
  // is what is asked: the spread of directions up to its outermost ring, and the horizon. And it
  // has to reach: the next ring out, at the band's own spacing, would aim past the highest a ring
  // may with its frame still below the pole, or a band of only the horizon would answer for
  // nothing.
  // At no overlap as well, which is where the check decides the spacing rather than the overlap.
  std::vector<CapturePlanSpec> lenses = PhoneLenses();
  for (CapturePlanSpec lens : PhoneLenses()) {
    lens.overlapTarget = 0.0;
    lenses.push_back(lens);
  }
  for (CapturePlanSpec lens : lenses) {
    lens.coverPoles = false;
    const CapturePlan plan = Plan(lens);
    double highest = 0.0;
    for (const CoverageNode& node : plan.nodes) {
      highest = std::max(highest, std::abs(Direction(node.targetOrientation).y));
    }
    // The next ring out, at the band's own spacing, would have crossed the limit. A band of one
    // ring has no spacing of its own, but it is never wider than the overlap's.
    const double limitDeg = 90.0 - lens.verticalFovDeg / 2.0;
    const double highestDeg = std::asin(highest) * kRadToDeg;
    double spacingDeg = lens.verticalFovDeg * (1.0 - lens.overlapTarget);
    for (const CoverageNode& node : plan.nodes) {
      const double elevationDeg = std::asin(Direction(node.targetOrientation).y) * kRadToDeg;
      if (elevationDeg > 1e-9) spacingDeg = std::min(spacingDeg, elevationDeg);
    }
    EXPECT_GT(highestDeg + spacingDeg, limitDeg - 1e-9)
        << lens.horizontalFovDeg << " by " << lens.verticalFovDeg << " at " << lens.overlapTarget;
    std::vector<Vec3> band;
    for (const Vec3& direction : SphereSamples(5000)) {
      if (std::abs(direction.y) <= highest) band.push_back(direction);
    }
    EXPECT_EQ(UncoveredInTheWorstCase(plan, AimsInTheCone(lens.acceptanceConeDeg), band), 0)
        << lens.horizontalFovDeg << " by " << lens.verticalFovDeg << ", " << plan.nodes.size()
        << " cells";
  }
}

TEST(CoveragePlanner, ABandWhoseLimitIsTheHorizonKeepsIt) {
  // A full 180 degree field leaves a band only its horizon ring. Counted from a pole, these ring
  // counts put that ring a few ulps above the horizon, the ring was dropped, the empty plan
  // failed its check and was laid again tighter than asked; a review found these (ADR 0076).
  for (const int halfRings : {39, 78, 156, 161, 169, 175}) {
    const double step = 90.0 / (halfRings - 0.3);
    CapturePlanSpec lens = Spec(180.0, 180.0, 1.0 - step / 180.0);
    lens.acceptanceConeDeg = 1.0;
    lens.coverPoles = false;
    const CapturePlan plan = Plan(lens);
    const double asked = std::ceil(360.0 / (180.0 * (1.0 - lens.overlapTarget)));
    EXPECT_EQ(plan.nodes.size(), static_cast<size_t>(asked)) << halfRings << " half rings";
  }
}

TEST(CoveragePlanner, LaysNoMoreCellsThanItNeeds) {
  // The check makes a plan complete; this keeps it from being complete by being dense. The page's
  // lens is complete as first laid, and a lens the first lay left holes in is tightened in small
  // steps: a coarser search laid 154 cells where 64 do (ADR 0076).
  EXPECT_EQ(Plan(ThePagesSpec()).nodes.size(), 32u);
  EXPECT_EQ(Plan(UprightSpec(66.0, 4.0 / 3.0, true)).nodes.size(), 38u);
  EXPECT_LE(Plan(UprightSpec(66.0, 16.0 / 9.0)).nodes.size(), 64u);
}

TEST(CoveragePlanner, RefusesAConeThatWouldNeedMoreCellsThanACaptureCouldHold) {
  // Short of being too wide to plan at all, a cone that is most of the frame needs tens of
  // thousands of cells, and laying and checking them took a minute in the browser before refusing
  // or answering (ADR 0076). Refused, quickly, and for the cone.
  CapturePlanSpec lens = ThePagesSpec();
  lens.acceptanceConeDeg = 24.6;
  RingsCoveragePlannerEngine planner;
  const Result<CapturePlan> plan = planner.Plan(lens, Intrinsics{});
  EXPECT_EQ(plan.status.code, StatusCode::InvalidArgument);
  EXPECT_NE(plan.status.detail.find("cone"), std::string::npos) << plan.status.detail;
  // Refused by the bound, not by the line: 24.6 is short of too wide for this lens.
  EXPECT_NE(plan.status.detail.find("more cells"), std::string::npos) << plan.status.detail;
}

TEST(CoveragePlanner, ADirectionIsCalledOpenOnlyOnceEveryCellNearItWasAsked) {
  // The check asks the cells nearest a direction first, and the cells it fails to ask can only make
  // it stricter, so a plan laid tighter than it needs to be still passes every completeness test.
  // These are plans where the covering cell is not the nearest: asking one side of each ring
  // only, or the nearest two cells, or skipping cells that are near enough, lays more of them.
  const auto cells = [](CapturePlanSpec lens, double coneDeg) {
    lens.acceptanceConeDeg = coneDeg;
    return Plan(lens).nodes.size();
  };
  EXPECT_EQ(cells(UprightSpec(27.5, 16.0 / 9.0, true), 4.0), 793u);
  EXPECT_EQ(cells(Spec(180.0, 101.25, 0.0), 43.0), 143u);
  EXPECT_EQ(cells(Spec(180.0, 101.25, 0.0), 47.0), 862u);
  EXPECT_EQ(cells(Spec(93.9, 110.0, 0.0), 1.0), 7u);
}

TEST(CoveragePlanner, TheBoundIsOnTheCellsLaid) {
  // Two long lenses either side of the bound: one whose plan is exactly 2000 cells, and one that
  // needs 2001. Compared with a grid of full circles instead, half again the cells a plan lays,
  // the first was refused, and so was every plan above about 1300 cells (ADR 0076).
  RingsCoveragePlannerEngine planner;
  const Result<CapturePlan> plan = planner.Plan(UprightSpec(20.7, 1.5), Intrinsics{});
  ASSERT_TRUE(plan.ok()) << plan.status.detail;
  EXPECT_EQ(plan.value.nodes.size(), 2000u);

  CapturePlanSpec oneMore = UprightSpec(20.7, 1.5, true);
  oneMore.overlapTarget = 0.0;
  const Result<CapturePlan> refused = planner.Plan(oneMore, Intrinsics{});
  EXPECT_EQ(refused.status.code, StatusCode::InvalidArgument);
  EXPECT_NE(refused.status.detail.find("more cells"), std::string::npos) << refused.status.detail;
}

TEST(CoveragePlanner, RefusesAConeTooWideForItsLens) {
  // A frame aimed at the edge of a cone half as wide as itself does not see its own cell's centre,
  // so no spacing closes the sphere and a plan would be a promise it cannot keep. The planner
  // checks a frame a degree smaller than the lens, so its line is a degree inside that.
  RingsCoveragePlannerEngine planner;
  CapturePlanSpec spec = Spec(20.0, 15.0, 0.30);
  spec.acceptanceConeDeg = 6.5;
  const Result<CapturePlan> plan = planner.Plan(spec, Intrinsics{});
  EXPECT_EQ(plan.status.code, StatusCode::InvalidArgument);
  EXPECT_NE(plan.status.detail.find("too wide"), std::string::npos) << plan.status.detail;
  // And from the other side: 4.4 degrees plans, the widest cone this lens plans at all.
  spec.acceptanceConeDeg = 4.4;
  EXPECT_TRUE(planner.Plan(spec, Intrinsics{}).ok()) << "a cone that leaves room still plans";
}

TEST(CoveragePlanner, TheFrameTestedIsTheRectangleACameraImages) {
  // The tests above are only as true as this helper. A camera images a rectangle in its image
  // plane, so the corner where the edges' angles meet is outside the frame and the corner of the
  // rectangle is inside; a box of yaw and pitch angles answers both the other way round.
  const double h = 50.0;
  const double v = 40.0;
  const auto from = [](double yaw, double pitch) {
    return Vec3{std::sin(yaw / kRadToDeg) * std::cos(pitch / kRadToDeg),
                std::sin(pitch / kRadToDeg),
                -std::cos(yaw / kRadToDeg) * std::cos(pitch / kRadToDeg)};
  };
  EXPECT_FALSE(InsideFieldOfView(Quat{}, from(h / 2.0 - 0.1, v / 2.0 - 0.1), h, v));
  const Vec3 corner{std::tan(h / 2.0 / kRadToDeg) * 0.999, std::tan(v / 2.0 / kRadToDeg) * 0.999,
                    -1.0};
  EXPECT_TRUE(InsideFieldOfView(Quat{}, Normalize(corner), h, v));
}

TEST(CoveragePlanner, StaysCompleteForAFrameRolledUpToTwentyFiveDegrees) {
  // The second place: a phone rolled about its view turns its frame's corners away from where the
  // plan put them. Twenty-five degrees either way costs nothing on the cell's centre at the page's
  // lens. These 5000 directions see nothing at thirty either, but a model with 20,000 finds one
  // there (ADR 0075), so this asks only what is known to hold.
  const CapturePlan plan = Plan(ThePagesSpec());
  std::vector<Quat> rolls;
  for (int degrees = -25; degrees <= 25; degrees += 5) {
    rolls.push_back(FromAxisAngle(Vec3{0.0, 0.0, 1.0}, degrees / kRadToDeg));
  }
  EXPECT_EQ(UncoveredInTheWorstCase(plan, rolls), 0) << plan.nodes.size() << " cells";
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
    // Bit for bit, because a build is keyed on the plan. Not `AngleBetween(...) < 1e-12`, which is
    // below the smallest angle it can report (2.98e-8 radians) and so asked for rounding luck: an
    // identical pair of plans read 2.98e-8 apart as soon as `Norm` rounded differently.
    const Quat& first = a.nodes[i].targetOrientation;
    const Quat& second = b.nodes[i].targetOrientation;
    EXPECT_EQ(first.w, second.w);
    EXPECT_EQ(first.x, second.x);
    EXPECT_EQ(first.y, second.y);
    EXPECT_EQ(first.z, second.z);
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

// A measured pose looking this way.
//
// Every test below the line is about the aim rule, and the aim rule applies only to an orientation
// something actually estimated — `confidence` zero is the contract's word for "nothing produced
// this", and `Locate` then targets by coverage alone (ADR 0042). So a test that means "the camera
// is pointing here" has to say so with a confidence, or it is quietly testing the sensorless path.
PoseSample Aiming(const Quat& orientation) {
  PoseSample sample;
  sample.orientation = orientation;
  sample.confidence = 1.0;
  return sample;
}

// ----------------------------------------------------------------------------------- locate

TEST(CoveragePlanner, LocateFindsTheCellTheUserIsAimingAt) {
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());
  for (const auto& node : plan.nodes) {
    auto guidance = planner.Locate(Aiming(node.targetOrientation), plan, CoverageState{});
    ASSERT_TRUE(guidance.ok());
    EXPECT_EQ(guidance.value.targetNode.value, node.id.value);
    EXPECT_NEAR(guidance.value.angularErrorDeg, 0.0, 1e-9);
    EXPECT_EQ(guidance.value.action, GuidanceAction::HoldStill);
  }
}

TEST(CoveragePlanner, ACellWhoseConeIsNotAMeasurementIsNeverTheOneBeingHeld) {
  // The other side of the guard `ArmBurst` grew in round 7, and the side that decides what the
  // user sees. `ICoveragePlannerEngine`'s own header says why they have to agree: "the reticle
  // would close on a cell that then would not arm — a capture that looks ready and does nothing,
  // which is worse to diagnose than one that says it is seeking".
  //
  // Both engines decided "inside the cone" with `angle > cone`, and that comparison is false for
  // `inf` and for `NaN` alike — so a cell carrying one was inside its cone from *every* direction.
  // Downstream, since ADR 0043, that is not a cosmetic wrong answer: the reticle closes, the dwell
  // matures, guidance says `Fire`, `ArmBurst` refuses the same cone as unusable, the dwell
  // restarts, and it repeats forever. ADR 0044 took the shutter away, so there is nothing left to
  // capture with while it does.
  //
  // **Both aims, and the second is the point.** This swept only a pose 90 degrees away, which
  // never reaches the `|| cone <= 0.0` clause: a reviewer deleted that clause from both engines
  // and all 529 tests stayed green. Zero and the negatives are refused by `angle > cone` when the
  // aim is wrong and only bite when it is exactly right — which is the arrangement that produces
  // the loop, since a cone of zero was where `Locate` said `HoldStill` and `ArmBurst` refused.
  //
  // Built by hand rather than planned, because both `Plan`s now refuse such a spec — which is the
  // point of the two guards, and also why nothing else in the suite can reach these lines.
  const double inf = std::numeric_limits<double>::infinity();
  const Quat cell = FromAzimuthElevation(0.0, 0.0);
  for (const double cone : {inf, -inf, std::numeric_limits<double>::quiet_NaN(), 0.0, -5.0}) {
    for (const Quat aim : {FromAzimuthElevation(90.0, 0.0), cell}) {
      CapturePlan plan;
      plan.spec = Spec();
      CoverageNode node;
      node.id = NodeId{1};
      node.targetOrientation = cell;
      node.acceptanceConeDeg = cone;
      plan.nodes.push_back(node);

      NullCoveragePlannerEngine nullEngine;
      RingsCoveragePlannerEngine ringsEngine;
      for (ICoveragePlannerEngine* engine :
           {static_cast<ICoveragePlannerEngine*>(&nullEngine),
            static_cast<ICoveragePlannerEngine*>(&ringsEngine)}) {
        auto guidance = engine->Locate(Aiming(aim), plan, CoverageState{});
        ASSERT_TRUE(guidance.ok()) << guidance.status.detail;
        EXPECT_NE(guidance.value.action, GuidanceAction::HoldStill)
            << "a cell was reported as held on a cone of " << cone
            << ", aimed " << (AngleBetween(aim, cell) < 1e-9 ? "straight at it" : "90 degrees off");
      }
    }
  }
}

TEST(CoveragePlanner, ACellPointingNowhereIsNeverTheOneBeingHeld) {
  // The same sentence about the other half of a node. `AngleBetweenDirections` answers a
  // degenerate direction with `0.0` — "dead on" — so a node whose `targetOrientation` is not a
  // rotation is inside an ordinary five-degree cone from every direction in the world. The cone is
  // a perfectly good measurement here; it is the *target* that is not one, and the two guards
  // added for the cone did not cover it.
  //
  // `0.0` is the sentinel this lens is about: it is also the answer for a camera pointed exactly
  // at the cell, so nothing downstream can tell "dead on" from "I could not say".
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  for (const Quat broken : {Quat{0, 0, 0, 0}, Quat{nan, 0, 0, 0}, Quat{inf, 0, 0, 0},
                            Quat{1, 0, nan, 0}}) {
    CapturePlan plan;
    plan.spec = Spec();
    CoverageNode node;
    node.id = NodeId{1};
    node.targetOrientation = broken;
    node.acceptanceConeDeg = 5.0;
    plan.nodes.push_back(node);

    NullCoveragePlannerEngine nullEngine;
    RingsCoveragePlannerEngine ringsEngine;
    for (ICoveragePlannerEngine* engine :
         {static_cast<ICoveragePlannerEngine*>(&nullEngine),
          static_cast<ICoveragePlannerEngine*>(&ringsEngine)}) {
      // Aimed at the identity, which is where a degenerate target now resolves to. It used to be
      // (90, 20), and that stopped testing anything once this branch hardened `Normalize(Quat)`:
      // `Direction(Quat{0,0,0,0})` became `(0,0,-1)`, an ordinary direction 90 degrees away, so
      // the cone refused the cell and both planners' `IsUsableRotation` guards could be deleted
      // with all 553 native tests green. Aiming here restores the 0.0 the test is named for.
      auto guidance = engine->Locate(Aiming(Quat{}), plan, CoverageState{});
      ASSERT_TRUE(guidance.ok()) << guidance.status.detail;
      EXPECT_NE(guidance.value.action, GuidanceAction::HoldStill)
          << "a cell that points nowhere was reported as held";
    }
  }
}

TEST(CoveragePlanner, LocateAsksTheUserToKeepLookingWhenTheyAreOff) {
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());
  const Quat aim = Multiply(plan.nodes.front().targetOrientation, FromAzimuthElevation(20.0, 0.0));
  auto guidance = planner.Locate(Aiming(aim), plan, CoverageState{});
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
    auto guidance = planner.Locate(Aiming(FromAzimuthElevation(azimuth, elevation)), plan, CoverageState{});
    ASSERT_TRUE(guidance.ok());
    EXPECT_LT(guidance.value.angularErrorDeg, plan.spec.horizontalFovDeg);
  }
}

// An attitude nudged off a cell by more than its acceptance cone, so the camera is aimed at no
// cell at all. Guidance behaves differently inside a cone and outside every one of them, and a
// test about seeking has to stand outside or it is testing the other rule.
//
// It checks that rather than assuming it. "More than its acceptance cone" was true of the nudge
// only because 20° happened to clear a 5° cone written down 180 lines away in `Spec()`, and a cell
// spacing nobody measured — narrow the cells or widen the cone and every caller quietly starts
// testing the inside-a-cone rule while its name still says otherwise.
Quat NudgedOffTarget(const CapturePlan& plan, const Quat& from, double degrees) {
  const Quat aim = Multiply(from, FromAxisAngle(Vec3{0.0, 1.0, 0.0}, degrees / kRadToDeg));
  for (const auto& node : plan.nodes) {
    const double offBy =
        AngleBetweenDirections(Direction(aim), Direction(node.targetOrientation)) * kRadToDeg;
    EXPECT_GT(offBy, node.acceptanceConeDeg)
        << "the nudge lands inside cell " << node.id.value << "'s cone, so the caller is not "
        << "testing the outside-every-cone rule it believes it is";
  }
  return aim;
}

// Everything captured except the named cells, which is what a session part-way through looks like.
CoverageState AllDoneBut(const CapturePlan& plan, std::vector<uint64_t> missing) {
  CoverageState state;
  state.nodesTotal = static_cast<int32_t>(plan.nodes.size());
  for (const auto& node : plan.nodes) {
    const bool hole = std::find(missing.begin(), missing.end(), node.id.value) != missing.end();
    if (hole) state.holes.push_back(node.id);
    else ++state.nodesSatisfied;
  }
  return state;
}

TEST(CoveragePlanner, LocateNamesTheCellTheCameraIsInsideEvenWhenItIsCaptured) {
  // The cell you are pointing at is the cell guidance names, captured or not. This reverses an
  // earlier rule — skip a captured cell and name the nearest missing one — which was right about
  // its own concern and wrong about the one that matters more on a phone. Standing still and
  // pressing capture three times filled the cell in front of you and then two neighbours, because
  // the target moved out from under a phone that had not moved; and since a burst records
  // whatever the camera sees, those two neighbours were filled with this cell's pixels.
  //
  // What the old rule was protecting is kept by the *action*, not by the target: it refuses to say
  // "hold still" at a cell there is no reason to re-shoot. It says the cell is done and lets the
  // user decide, which is what makes a deliberate re-capture possible at all.
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());
  const CoverageNode& aimedAt = plan.nodes.front();
  const CoverageNode& missing = plan.nodes.back();

  auto guidance = planner.Locate(Aiming(aimedAt.targetOrientation), plan,
                                 AllDoneBut(plan, {missing.id.value}));
  ASSERT_TRUE(guidance.ok());
  EXPECT_EQ(guidance.value.targetNode.value, aimedAt.id.value);
  EXPECT_EQ(guidance.value.action, GuidanceAction::AlreadyCaptured);
  EXPECT_LE(guidance.value.angularErrorDeg, aimedAt.acceptanceConeDeg);
}

TEST(CoveragePlanner, LocateSendsYouOnWhenYouAreAimedAtNoCellAtAll) {
  // The other half of the rule above, and the one that keeps a capture moving: outside every
  // acceptance cone there is no cell to hold on, so the nearest *missing* one is the answer.
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());
  const CoverageNode& aimedAt = plan.nodes.front();
  const CoverageNode& missing = plan.nodes.back();
  const Quat between = NudgedOffTarget(plan, aimedAt.targetOrientation, 20.0);

  auto guidance = planner.Locate(Aiming(between), plan, AllDoneBut(plan, {missing.id.value}));
  ASSERT_TRUE(guidance.ok());
  EXPECT_EQ(guidance.value.targetNode.value, missing.id.value);
  EXPECT_EQ(guidance.value.action, GuidanceAction::Seek);
  // The error is measured to the cell that was named, and pinned rather than merely asserted
  // positive: this is the number the reticle's radius is drawn from on every frame of a real
  // capture, and it is computed on a line this change rewrote. `EXPECT_GT(..., 0.0)` passed for a
  // constant.
  EXPECT_NEAR(guidance.value.angularErrorDeg,
              AngleBetweenDirections(Direction(between), Direction(missing.targetOrientation)) *
                  kRadToDeg,
              1e-9);
}

TEST(CoveragePlanner, LocateSaysHoldStillOnACellThatStillNeedsShooting) {
  // Inside a cone and the cell is a hole: this is the one case that asks for a capture, and it is
  // what the eventual dwell trigger will fire on. Told apart from the captured case above by the
  // action alone, since both name the cell under the reticle.
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());
  const CoverageNode& aimedAt = plan.nodes.front();

  auto guidance = planner.Locate(Aiming(aimedAt.targetOrientation), plan,
                                 AllDoneBut(plan, {aimedAt.id.value}));
  ASSERT_TRUE(guidance.ok());
  EXPECT_EQ(guidance.value.targetNode.value, aimedAt.id.value);
  EXPECT_EQ(guidance.value.action, GuidanceAction::HoldStill);
}

TEST(CoveragePlanner, AConeNobodyCouldMeasureAgainstIsRefusedRatherThanPlanned) {
  // Every non-finite value, not one of them, and that is the whole point of this test rather than
  // thoroughness for its own sake. Two attempts at this guard each closed one member of the class
  // and left another: `<= 0.0` admitted NaN, and `!(x > 0.0)` admitted infinity — which is the
  // worse one, because a cone of `inf` is not merely unmeasurable, it makes `Locate` report every
  // cell as one the camera is inside and `ArmBurst` accept a burst 179 degrees off. Verbatim the
  // "every cell armable from anywhere" both fixes claimed to close.
  //
  // Both engines, because they disagreed twice: the rings planner has asked `std::isfinite` since
  // it was written and the null one has now been wrong about the same class in two different
  // directions. The null one is what every manager test runs on, so its disagreement is the one
  // the suite cannot see.
  for (const double cone : {std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity()}) {
    CapturePlanSpec spec;
    spec.horizontalFovDeg = 66.0;
    spec.verticalFovDeg = 50.0;
    spec.overlapTarget = 0.30;
    spec.acceptanceConeDeg = cone;
    Intrinsics lens;
    lens.width = 1280;
    lens.height = 960;

    NullCoveragePlannerEngine null;
    EXPECT_EQ(null.Plan(spec, lens).status.code, StatusCode::InvalidArgument)
        << "the null planner accepted a cone of " << cone;
    RingsCoveragePlannerEngine rings;
    EXPECT_EQ(rings.Plan(spec, lens).status.code, StatusCode::InvalidArgument)
        << "the rings planner accepted a cone of " << cone;
  }
}

TEST(CoveragePlanner, WithNoAimTheTargetIsTheNearestMissingCellRatherThanWhateverSitsAtIdentity) {
  // UC-4: a phone that declined motion, or has none, tracks vision-only and reports identity for
  // the life of the session at confidence zero — the contract's word for "nothing estimated this".
  //
  // Preferring the cell under the camera is only meaningful when there *is* a camera direction.
  // Against an unmeasured identity it means preferring whichever cell happens to sit at identity,
  // on every tick, for ever: capture that cell and guidance says `AlreadyCaptured` about it until
  // the session ends, and the sphere can never move on. So with no aim, coverage decides alone and
  // the answer is ADR 0027's — the nearest cell still missing (ADR 0042).
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());
  const CoverageNode& atIdentity = plan.nodes.front();
  const CoverageNode& missing = plan.nodes.back();
  ASSERT_NE(atIdentity.id.value, missing.id.value);

  PoseSample unmeasured;
  unmeasured.orientation = atIdentity.targetOrientation;   // exactly where the aim rule would bite
  unmeasured.confidence = 0.0;

  auto guidance = planner.Locate(unmeasured, plan, AllDoneBut(plan, {missing.id.value}));
  ASSERT_TRUE(guidance.ok());
  EXPECT_EQ(guidance.value.targetNode.value, missing.id.value)
      << "with no aim to prefer, a captured cell under an unmeasured identity still won the target";
  EXPECT_EQ(guidance.value.action, GuidanceAction::Seek);

  // And the same attitude *with* a confidence keeps the aim rule, so this is about the measurement
  // and not about the direction.
  auto aimed = planner.Locate(Aiming(atIdentity.targetOrientation), plan,
                              AllDoneBut(plan, {missing.id.value}));
  ASSERT_TRUE(aimed.ok());
  EXPECT_EQ(aimed.value.targetNode.value, atIdentity.id.value);
  EXPECT_EQ(aimed.value.action, GuidanceAction::AlreadyCaptured);
}

TEST(CoveragePlanner, LocateStillPicksTheNearestOfTheCellsThatAreMissing) {
  // Skipping the captured ones must not turn into ignoring distance: of what is left, the closest
  // is still the one to send someone to.
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());
  ASSERT_GE(plan.nodes.size(), 3u);
  // From outside every cone, so this is the seeking rule rather than the aimed-at-a-cell one.
  const Quat looking = NudgedOffTarget(plan, plan.nodes[0].targetOrientation, 20.0);
  const auto angleTo = [&](const CoverageNode& node) {
    return AngleBetweenDirections(Direction(looking), Direction(node.targetOrientation));
  };

  // The far cell has to come *earlier* in plan order than the near one, or the test cannot tell
  // "nearest" from "first one scanned" — and a `nearestOf` that simply kept its first hit would
  // pass. The previous fixture took `nodes[1]` as the near cell and `nodes[size/2]` as the far
  // one, so plan order and distance agreed and the assertion proved nothing.
  size_t farIdx = plan.nodes.size();
  size_t nearIdx = plan.nodes.size();
  for (size_t i = 0; i < plan.nodes.size() && nearIdx == plan.nodes.size(); ++i) {
    for (size_t j = i + 1; j < plan.nodes.size(); ++j) {
      if (angleTo(plan.nodes[j]) < angleTo(plan.nodes[i])) {
        farIdx = i;
        nearIdx = j;
        break;
      }
    }
  }
  ASSERT_LT(nearIdx, plan.nodes.size())
      << "this plan is already sorted by distance from the camera, so nothing here can tell the "
      << "two rules apart";
  const CoverageNode& near = plan.nodes[nearIdx];
  const CoverageNode& far = plan.nodes[farIdx];

  auto guidance = planner.Locate(Aiming(looking), plan,
                                 AllDoneBut(plan, {near.id.value, far.id.value}));
  ASSERT_TRUE(guidance.ok());
  ASSERT_LT(angleTo(near), angleTo(far));
  ASSERT_LT(farIdx, nearIdx);
  EXPECT_EQ(guidance.value.targetNode.value, near.id.value);
}

TEST(CoveragePlanner, LocateSaysTheSphereIsDoneWhenNothingIsMissing) {
  // The action existed and nothing ever produced it, so a finished sphere went on asking for the
  // cell nearest the phone forever.
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());
  auto guidance = planner.Locate(Aiming(plan.nodes.front().targetOrientation), plan, AllDoneBut(plan, {}));
  ASSERT_TRUE(guidance.ok());
  EXPECT_EQ(guidance.value.action, GuidanceAction::SphereDone);
}

TEST(CoveragePlanner, LocateAimsAtTheNearestCellWhenNothingHasBeenCaptured) {
  // The start of every session: no holes recorded yet is not the same as nothing missing, and
  // reading an empty coverage state as a finished sphere would end a capture before it began.
  RingsCoveragePlannerEngine planner;
  const CapturePlan plan = Plan(Spec());
  const CoverageNode& aimedAt = plan.nodes.front();
  auto guidance = planner.Locate(Aiming(aimedAt.targetOrientation), plan, CoverageState{});
  ASSERT_TRUE(guidance.ok());
  EXPECT_EQ(guidance.value.targetNode.value, aimedAt.id.value);
  EXPECT_EQ(guidance.value.action, GuidanceAction::HoldStill);
}

TEST(CoveragePlanner, LocateRefusesAnEmptyPlan) {
  RingsCoveragePlannerEngine planner;
  EXPECT_EQ(planner.Locate(Aiming(Quat{}), CapturePlan{}, CoverageState{}).status.code,
            StatusCode::FailedPrecondition);
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

  // Both of those are refused first now as too narrow for their cone (ADR 0076), so the ring
  // count is reached by a step that is fine for another reason: an overlap a hair short of one.
  spec = Spec();
  spec.overlapTarget = 1.0 - 1e-12;
  const Result<CapturePlan> plan = engine.Plan(spec, Intrinsics{});
  EXPECT_EQ(plan.status.code, StatusCode::InvalidArgument);
  EXPECT_NE(plan.status.detail.find("more cells"), std::string::npos) << plan.status.detail;
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

  // A cell on the horizon, because a cell at a pole owes no roll at all (ADR 0072).
  const auto onTheHorizon = std::find_if(plan.nodes.begin(), plan.nodes.end(), [](const auto& n) {
    return std::abs(Direction(n.targetOrientation).y) < 1e-12;
  });
  ASSERT_NE(onTheHorizon, plan.nodes.end());
  const Quat aimed = onTheHorizon->targetOrientation;
  auto straight = engine.Locate(Aiming(aimed), plan, CoverageState{});
  ASSERT_TRUE(straight.ok());

  // The same direction, rotated 30 degrees about the axis the camera looks along.
  const Vec3 axis = Direction(aimed);
  const Quat rolled = Multiply(FromAxisAngle(axis, 30.0 / kRadToDeg), aimed);
  auto tilted = engine.Locate(Aiming(rolled), plan, CoverageState{});
  ASSERT_TRUE(tilted.ok());

  EXPECT_EQ(tilted.value.targetNode.value, straight.value.targetNode.value);
  EXPECT_NEAR(tilted.value.angularErrorDeg, straight.value.angularErrorDeg, 1e-6);
  EXPECT_NEAR(tilted.value.rollErrorDeg, 30.0, 1e-6);
}

TEST(CoveragePlanner, ReportsNoRollWhenTheCameraIsUpright) {
  // On the horizon, because at a pole every roll reads zero (ADR 0072), which would prove nothing.
  RingsCoveragePlannerEngine engine;
  const CapturePlan plan = Plan(Spec());
  const auto onTheHorizon = std::find_if(plan.nodes.begin(), plan.nodes.end(), [](const auto& n) {
    return std::abs(Direction(n.targetOrientation).y) < 1e-12;
  });
  ASSERT_NE(onTheHorizon, plan.nodes.end());
  auto guidance = engine.Locate(Aiming(onTheHorizon->targetOrientation), plan, CoverageState{});
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
    auto guidance = engine.Locate(Aiming(rolled), plan, CoverageState{});
    ASSERT_TRUE(guidance.ok());
    EXPECT_EQ(guidance.value.targetNode.value, node.id.value);
    EXPECT_NEAR(guidance.value.angularErrorDeg, 0.0, 1e-6);
  }
}


TEST(NullCoveragePlanner, AlsoSeparatesRollFromAim) {
  // The null engine is what the manager tests run against, so guidance semantics that differ
  // between it and the real one make those tests assert something the shipped path does not do.
  // It recreates the "reticle never closes when the phone is rolled" failure otherwise.
  NullCoveragePlannerEngine engine;
  CapturePlan plan;
  CoverageNode node;
  node.id = NodeId{1};
  node.targetOrientation = FromAzimuthElevation(25.0, -10.0);
  node.acceptanceConeDeg = 5.0;
  plan.nodes.push_back(node);

  auto straight = engine.Locate(Aiming(node.targetOrientation), plan, CoverageState{});
  ASSERT_TRUE(straight.ok());
  EXPECT_NEAR(straight.value.angularErrorDeg, 0.0, 1e-9);
  EXPECT_NEAR(straight.value.rollErrorDeg, 0.0, 1e-9);

  const Vec3 axis = Direction(node.targetOrientation);
  const Quat rolled = Multiply(FromAxisAngle(axis, 30.0 / kRadToDeg), node.targetOrientation);
  auto tilted = engine.Locate(Aiming(rolled), plan, CoverageState{});
  ASSERT_TRUE(tilted.ok());
  EXPECT_NEAR(tilted.value.angularErrorDeg, 0.0, 1e-6);
  EXPECT_NEAR(tilted.value.rollErrorDeg, 30.0, 1e-6);
  EXPECT_EQ(tilted.value.action, GuidanceAction::HoldStill);
}

// The roll guidance, which both engines give the same way (ADR 0072): how far the phone is rolled
// from level where it looks, faded out near straight up or down, whichever cell is the target.
// Asserted at the engine for each, because the utilities' own tests cannot see what a planner does
// with the number.

CapturePlan OneCellAt(double azimuthDeg, double elevationDeg) {
  CapturePlan plan;
  CoverageNode node;
  node.id = NodeId{1};
  node.targetOrientation = FromAzimuthElevation(azimuthDeg, elevationDeg);
  node.acceptanceConeDeg = 5.0;
  plan.nodes.push_back(node);
  return plan;
}

double RollDeg(ICoveragePlannerEngine& engine, const Quat& phone, const CapturePlan& plan) {
  auto guidance = engine.Locate(Aiming(phone), plan, CoverageState{});
  EXPECT_TRUE(guidance.ok());
  return guidance.value.rollErrorDeg;
}

// Turned about its own view, which is what rolling a phone is.
Quat Rolled(const Quat& phone, double degrees) {
  return Multiply(FromAxisAngle(Direction(phone), degrees / kRadToDeg), phone);
}

TEST(CoveragePlanner, TheRollIsHowFarThePhoneIsFromLevelWhereverTheCellIs) {
  // Level reads zero and a phone rolled 25 degrees reads 25, on the cell, four degrees beside it,
  // and far from it. Four degrees off a cell sixty up is where the twist left after the shortest
  // turn onto the cell is largest inside the cone — 7.0 degrees — so a planner still reporting
  // that twist near the cell reads it there.
  NullCoveragePlannerEngine null;
  RingsCoveragePlannerEngine rings;
  const CapturePlan plan = OneCellAt(0.0, 60.0);
  const Quat target = plan.nodes.front().targetOrientation;
  ASSERT_GT(std::abs(RollBetween(FromAzimuthElevation(8.0, 60.0), target) * kRadToDeg), 3.0);
  for (ICoveragePlannerEngine* engine : {static_cast<ICoveragePlannerEngine*>(&null),
                                         static_cast<ICoveragePlannerEngine*>(&rings)}) {
    for (const auto& [az, el] : {std::pair{0.0, 60.0}, std::pair{8.0, 60.0}, std::pair{0.0, 56.0},
                                 std::pair{180.0, -30.0}, std::pair{170.0, 10.0},
                                 std::pair{90.0, 60.0}}) {
      const Quat level = FromAzimuthElevation(az, el);
      EXPECT_NEAR(RollDeg(*engine, level, plan), 0.0, 1e-9) << az << "," << el;
      EXPECT_NEAR(RollDeg(*engine, Rolled(level, 25.0), plan), 25.0, 1e-6) << az << "," << el;
    }
  }
}

TEST(CoveragePlanner, TheRollDoesNotDependOnWhichCellIsTheTarget) {
  // A roll that depends on the target jumps wherever the target changes, which is every boundary
  // between two missing cells: on a narrow lens those lie a few degrees from both. Phones rolled
  // by various amounts, everywhere, against cells at every elevation a plan of one to nine
  // half-rings lays — the ring at exactly 75 and the poles included.
  NullCoveragePlannerEngine null;
  RingsCoveragePlannerEngine rings;
  std::vector<double> elevations;
  for (int halfRings = 1; halfRings <= 9; ++halfRings) {
    for (int ring = -halfRings; ring <= halfRings; ++ring) {
      elevations.push_back(90.0 * ring / halfRings);
    }
  }
  for (ICoveragePlannerEngine* engine : {static_cast<ICoveragePlannerEngine*>(&null),
                                         static_cast<ICoveragePlannerEngine*>(&rings)}) {
    for (const double el : {-88.0, -80.0, -75.0, -40.0, 0.0, 33.0, 72.0, 79.0, 86.0}) {
      for (const double roll : {0.0, 30.0, -100.0, 170.0}) {
        const Quat phone = Rolled(FromAzimuthElevation(el * 3.0, el), roll);
        const double reference = RollDeg(*engine, phone, OneCellAt(0.0, 0.0));
        for (const double cellEl : elevations) {
          for (const double cellAz : {0.0, 72.0, 144.0, -144.0}) {
            EXPECT_EQ(RollDeg(*engine, phone, OneCellAt(cellAz, cellEl)), reference)
                << el << " " << roll << " against " << cellAz << "," << cellEl;
          }
        }
      }
    }
  }
}

TEST(CoveragePlanner, TheRollIsSteadyWhereTheTargetChangesCell) {
  // What target independence buys, through `Locate`: a phone rolled thirty degrees, turned along
  // the ring between cells of a narrow lens, every one of them missing, so the target changes as it
  // goes. Rolled, because a level phone reads zero against any target and a reading scaled by the
  // target would pass. Under the cell-relative blend a level phone read -15.7 and then +15.7 a
  // twentieth of a degree later.
  RingsCoveragePlannerEngine rings;
  const CapturePlan plan = Plan(Spec(35.0, 26.0, 0.30));
  for (const double el : {-75.0, -50.0, 15.0, 70.0}) {
    std::set<uint64_t> targets;
    double first = 0.0;
    double largestDeparture = 0.0;
    for (int step = 0; step <= 7200; ++step) {
      const Quat phone = Rolled(FromAzimuthElevation(0.05 * step, el), 30.0);
      auto guidance = rings.Locate(Aiming(phone), plan, CoverageState{});
      ASSERT_TRUE(guidance.ok());
      targets.insert(guidance.value.targetNode.value);
      if (step == 0) first = guidance.value.rollErrorDeg;
      largestDeparture =
          std::max(largestDeparture, std::abs(guidance.value.rollErrorDeg - first));
    }
    EXPECT_GT(targets.size(), 3u) << el;
    EXPECT_GT(std::abs(first), 25.0) << el;
    EXPECT_LT(largestDeparture, 1e-9) << el;
  }
}

TEST(CoveragePlanner, NearAPoleLevelFadesFromAllOfItAtFifteenDegreesToNoneAtFive) {
  // Level has no meaning looking straight up or down, and the roll from it spins faster the closer
  // the phone gets. A phone rolled forty degrees reads all of it fifteen degrees from the pole,
  // none of it within five, and a straight line between — against a cell on the horizon and
  // against the pole cell alike.
  NullCoveragePlannerEngine null;
  RingsCoveragePlannerEngine rings;
  for (const double sign : {1.0, -1.0}) {
    for (const CapturePlan& plan : {OneCellAt(0.0, 0.0), OneCellAt(0.0, sign * 90.0)}) {
      for (const double fromPole : {0.0, 4.9, 5.1, 7.5, 10.0, 12.5, 14.9, 15.1, 20.0}) {
        const Quat phone = Rolled(FromAzimuthElevation(30.0, sign * (90.0 - fromPole)), 40.0);
        const double expected = 40.0 * std::clamp((fromPole - 5.0) / 10.0, 0.0, 1.0);
        for (ICoveragePlannerEngine* engine : {static_cast<ICoveragePlannerEngine*>(&null),
                                               static_cast<ICoveragePlannerEngine*>(&rings)}) {
          EXPECT_NEAR(RollDeg(*engine, phone, plan), expected, 1e-6) << sign << " " << fromPole;
        }
      }
    }
  }
}

TEST(CoveragePlanner, AimedAtACellStraightUpOrDownAnyRollReadsZero) {
  // A frame looking straight up has no horizon to keep, and turning it about its view changes
  // nothing a stitch needs.
  NullCoveragePlannerEngine null;
  RingsCoveragePlannerEngine rings;
  for (ICoveragePlannerEngine* engine : {static_cast<ICoveragePlannerEngine*>(&null),
                                         static_cast<ICoveragePlannerEngine*>(&rings)}) {
    for (const double sign : {1.0, -1.0}) {
      const CapturePlan pole = OneCellAt(0.0, sign * 90.0);
      for (const double roll : {0.0, 30.0, 135.0}) {
        EXPECT_NEAR(RollDeg(*engine, Rolled(pole.nodes.front().targetOrientation, roll), pole), 0.0,
                    1e-9)
            << sign << " " << roll;
      }
    }
  }
}

TEST(CoveragePlanner, APhoneTippedOverStraightUpOrDownKeepsAMeaningfulHorizon) {
  // What the fade is for. A phone rolled thirty degrees and pitched over the zenith, or under the
  // nadir, passes through every heading at once; unfaded, the roll from level turns half a turn in
  // a twentieth of a degree there.
  NullCoveragePlannerEngine engine;
  const CapturePlan plan = OneCellAt(90.0, 0.0);
  for (const double sign : {1.0, -1.0}) {
    const Quat start = Rolled(FromAzimuthElevation(0.0, sign * 60.0), 30.0);
    const auto at = [&](int step) {
      return Multiply(FromAxisAngle(Vec3{1.0, 0.0, 0.0}, sign * 0.05 * step / kRadToDeg), start);
    };
    ASSERT_GT(Direction(at(600)).y * sign, 0.9999);
    double previous = RollDeg(engine, at(0), plan);
    double largestStep = 0.0;
    for (int step = 1; step <= 1200; ++step) {
      const double roll = RollDeg(engine, at(step), plan);
      largestStep = std::max(largestStep, std::abs(std::remainder(roll - previous, 360.0)));
      previous = roll;
    }
    EXPECT_LT(largestStep, 2.0) << sign;
  }
}

TEST(NullCoveragePlanner, SaysTheSameFourThingsAboutAimAndCoverageTheRealOneDoes) {
  // Every action this engine can return, asserted here because nothing else asserts them. It is
  // the engine the manager tests run against, so its guidance rule is what those tests believe the
  // shipped path does — and three of the four actions could have been replaced with the wrong one
  // and left the whole suite green. A rule that agrees with the real planner by intention and not
  // by test is a rule that will drift the next time one of them is edited.
  NullCoveragePlannerEngine engine;
  CapturePlan plan;
  CoverageNode here;
  here.id = NodeId{1};
  here.targetOrientation = Quat{};
  here.acceptanceConeDeg = 5.0;
  plan.nodes.push_back(here);
  CoverageNode there;
  there.id = NodeId{2};
  there.targetOrientation = FromAzimuthElevation(40.0, 0.0);
  there.acceptanceConeDeg = 5.0;
  plan.nodes.push_back(there);

  // Nothing evaluated yet: coverage has no opinion, so being on a cell means shoot it.
  auto fresh = engine.Locate(Aiming(Quat{}), plan, CoverageState{});
  ASSERT_TRUE(fresh.ok());
  EXPECT_EQ(fresh.value.targetNode.value, here.id.value);
  EXPECT_EQ(fresh.value.action, GuidanceAction::HoldStill);

  // On a cell that is still a hole.
  auto needed = engine.Locate(Aiming(Quat{}), plan, AllDoneBut(plan, {here.id.value}));
  ASSERT_TRUE(needed.ok());
  EXPECT_EQ(needed.value.targetNode.value, here.id.value);
  EXPECT_EQ(needed.value.action, GuidanceAction::HoldStill);

  // On a cell that is already shot: still named — that is the whole point of the aim rule — but
  // the action says there is no reason to shoot it again, which is what the page colours the
  // reticle from and what stops a dwell trigger firing on a cell nobody asked to re-take.
  auto done = engine.Locate(Aiming(Quat{}), plan, AllDoneBut(plan, {there.id.value}));
  ASSERT_TRUE(done.ok());
  EXPECT_EQ(done.value.targetNode.value, here.id.value);
  EXPECT_EQ(done.value.action, GuidanceAction::AlreadyCaptured);

  // Between the two cells, inside neither cone: the nearest cell that is still missing.
  const Quat between = FromAzimuthElevation(20.0, 0.0);
  auto seeking = engine.Locate(Aiming(between), plan, AllDoneBut(plan, {there.id.value}));
  ASSERT_TRUE(seeking.ok());
  EXPECT_EQ(seeking.value.targetNode.value, there.id.value);
  EXPECT_EQ(seeking.value.action, GuidanceAction::Seek);

  // Nothing left anywhere.
  auto finished = engine.Locate(Aiming(Quat{}), plan, AllDoneBut(plan, {}));
  ASSERT_TRUE(finished.ok());
  EXPECT_EQ(finished.value.action, GuidanceAction::SphereDone);

  // And the fifth thing, which is not an action: whether there was an aim to speak of. Asserted
  // here for the same reason as the four above — this engine is unreachable from the browser, so
  // nothing else can catch it. Forcing `aimKnown` true left the whole suite green while the rings
  // engine's copy of the same line failed thirteen browser tests, which is exactly the asymmetry
  // that lets a null implementation drift away from the real one.
  EXPECT_TRUE(fresh.value.aimKnown);
  PoseSample unaimed;
  unaimed.confidence = 0.0;
  auto blind = engine.Locate(unaimed, plan, CoverageState{});
  ASSERT_TRUE(blind.ok());
  EXPECT_FALSE(blind.value.aimKnown);
  // And with no aim, coverage decides alone (ADR 0042): the nearest missing cell rather than
  // whatever sits at identity, and never `HoldStill` — a page gating its shutter on that would
  // offer nothing for ever.
  EXPECT_EQ(blind.value.action, GuidanceAction::Seek);
}


// ------------------------------------------------------------------------------ gaps
//
// After a build the frames are where the solve placed them, through the lens it fitted, and that
// is the only place a wrong lens or a drifting pose shows (ADR 0084). The invariant worth having is
// the one a user acts on: aim a frame at every gap reported, anywhere inside its cone, and the
// sphere closes.

Intrinsics LensOf(double h, double v) { return LensFromFieldOfView(h, v, 640, 480); }

std::vector<PlacedFrame> AtEveryCell(const CapturePlan& plan, const Intrinsics& lens) {
  std::vector<PlacedFrame> frames;
  for (const CoverageNode& node : plan.nodes) {
    frames.push_back(PlacedFrame{node.targetOrientation, lens});
  }
  return frames;
}

// The frames, and one frame aimed at each gap at the edge of its cone, which is as far off as a
// user holding still inside it can be. One, because a user takes one: four around the cone cover
// together what none does alone, and passed with a fill reach that forgot the cone. The side turns
// with each gap, so no one side is the only one asked. Neighbouring fill frames still cover for
// each other, so these do not pin how far one reaches; the refusal of a fill lens too narrow for
// the cone, below, does.
std::vector<PlacedFrame> Filled(std::vector<PlacedFrame> frames,
                                const std::vector<CoverageGap>& gaps, const Intrinsics& lens,
                                double coneDeg) {
  for (size_t g = 0; g < gaps.size(); ++g) {
    const double around = 2.0 * std::numbers::pi * static_cast<double>(g % 8) / 8.0;
    const Quat off = FromAxisAngle(Vec3{std::cos(around), std::sin(around), 0.0},
                                   coneDeg / kRadToDeg);
    frames.push_back(PlacedFrame{Multiply(gaps[g].target, off), lens});
  }
  return frames;
}

// Directions no frame covers, asked by this test rather than by the engine: `Project` alone, at
// directions laid between the engine's own. A gap is a region, and asking the engine's directions
// again cannot see a sliver of one that falls between them, which is where it would be.
int OpenBetweenTheSamples(const CapturePlan& plan, const std::vector<PlacedFrame>& frames) {
  double highest = 0.0;
  for (const CoverageNode& node : plan.nodes) {
    highest = std::max(highest, std::abs(Direction(node.targetOrientation).y));
  }
  std::vector<Vec3> asked;
  const double golden = std::numbers::pi * (3.0 - std::sqrt(5.0));
  constexpr int kCount = 20000;
  // Two lattices offset from the engine's by a quarter step either way, and the engine's own
  // turned by 0.6 degrees about an axis it has no symmetry about: off its directions by about
  // half their spacing on average, not exactly between them anywhere, which a lattice cannot be.
  const Quat turned = FromAxisAngle(Normalize(Vec3{0.3, 0.8, 0.5}), 0.6 / kRadToDeg);
  for (const double phase : {0.25, 0.75, 0.5}) {
    for (int i = 0; i < kCount; ++i) {
      const double y = 1.0 - 2.0 * (static_cast<double>(i) + phase) / kCount;
      const double radius = std::sqrt(std::max(0.0, 1.0 - y * y));
      const double theta = golden * (i + phase);
      Vec3 direction{std::cos(theta) * radius, y, std::sin(theta) * radius};
      if (phase == 0.5) direction = Rotate(turned, direction);
      if (std::abs(direction.y) > highest) continue;
      asked.push_back(direction);
    }
  }
  // And where the engine's spread thins, which is where it asks extra directions of its own: the
  // horizon, a band's edges just inside them, and the poles a few tenths of a degree out.
  const auto ringAt = [&](double y, int count) {
    const double radius = std::sqrt(std::max(0.0, 1.0 - y * y));
    for (int i = 0; i < count; ++i) {
      const double azimuth = 2.0 * std::numbers::pi * (i + 0.25) / count;
      asked.push_back(Vec3{std::sin(azimuth) * radius, y, -std::cos(azimuth) * radius});
    }
  };
  ringAt(0.0, 7200);
  if (highest >= 1.0 - 1e-9) {
    for (const double side : {1.0, -1.0}) {
      for (const double fromThePole : {0.3, 0.7, 1.2, 2.0, 2.8}) {
        ringAt(side * std::cos(fromThePole / kRadToDeg), 720);
      }
    }
  } else if (highest > 0.0) {
    for (const double inside : {0.0, 0.002, 0.005}) {
      ringAt(highest - inside, 3600);
      ringAt(-(highest - inside), 3600);
    }
  }
  int open = 0;
  for (const Vec3& direction : asked) {
    const bool seen = std::any_of(frames.begin(), frames.end(), [&](const PlacedFrame& frame) {
      if (Dot(Direction(frame.rotation), direction) < 0.3) return false;
      const ProjectedPixel at = Project(frame.lens, Rotate(Conjugate(frame.rotation), direction));
      return at.valid && at.pixel.x >= 0.0 && at.pixel.x <= frame.lens.width &&
             at.pixel.y >= 0.0 && at.pixel.y <= frame.lens.height;
    });
    if (!seen) ++open;
  }
  return open;
}

double Fraction(const std::vector<CoverageGap>& gaps) {
  double sum = 0.0;
  for (const CoverageGap& gap : gaps) sum += gap.solidAngleFraction;
  return sum;
}

TEST(CoverageGaps, AFrameAtEveryCellOfThePlannedLensLeavesNone) {
  const CapturePlan plan = Plan(Spec());
  RingsCoveragePlannerEngine engine;
  auto gaps = engine.FindGaps(plan, AtEveryCell(plan, LensOf(66.0, 50.0)), LensOf(66.0, 50.0));
  ASSERT_TRUE(gaps.ok()) << gaps.status.detail;
  EXPECT_TRUE(gaps.value.empty()) << gaps.value.size() << " gaps, " << Fraction(gaps.value);
}

TEST(CoverageGaps, AMissingFrameLeavesAGapWhereItWouldHaveLookedAndFillingItCloses) {
  const CapturePlan plan = Plan(Spec());
  const Intrinsics lens = LensOf(66.0, 50.0);
  std::vector<PlacedFrame> frames = AtEveryCell(plan, lens);
  // A cell off every axis the plan is symmetric about, so a target mirrored in azimuth or in
  // elevation is not near it: at the centre of the sphere's front, either mirror was the same
  // cell.
  const auto missing =
      std::find_if(plan.nodes.begin(), plan.nodes.end(), [](const CoverageNode& n) {
        const Vec3 at = Direction(n.targetOrientation);
        const double azimuth = std::atan2(-at.x, -at.z) * kRadToDeg;
        const double elevation = ElevationOf(n.targetOrientation);
        return elevation > 20.0 && elevation < 70.0 && azimuth > 20.0 && azimuth < 70.0;
      });
  ASSERT_NE(missing, plan.nodes.end());
  const Vec3 there = Direction(missing->targetOrientation);
  frames.erase(frames.begin() + (missing - plan.nodes.begin()));

  RingsCoveragePlannerEngine engine;
  auto gaps = engine.FindGaps(plan, frames, lens);
  ASSERT_TRUE(gaps.ok()) << gaps.status.detail;
  ASSERT_FALSE(gaps.value.empty());
  // Every gap is within the missing frame's reach of where it looked, since nothing else is gone.
  for (const CoverageGap& gap : gaps.value) {
    const Vec3 at = Direction(gap.target);
    const double degrees = std::acos(std::clamp(at.x * there.x + at.y * there.y + at.z * there.z,
                                                -1.0, 1.0)) * kRadToDeg;
    EXPECT_LT(degrees, 42.0);
  }
  EXPECT_GT(Fraction(gaps.value), 0.0);

  const std::vector<PlacedFrame> filled = Filled(frames, gaps.value, lens, 5.0);
  auto after = engine.FindGaps(plan, filled, lens);
  ASSERT_TRUE(after.ok()) << after.status.detail;
  EXPECT_TRUE(after.value.empty()) << after.value.size() << " left";
  EXPECT_EQ(OpenBetweenTheSamples(plan, filled), 0);
}

// The field report's cause: the page's 66-degree guess against a camera that sees less. The plan
// was complete for the lens it was laid with, and the frames are not that lens. Aimed at the
// cells' centres, frames 50 degrees across leave none; 46 leave nothing open, but seams thinner
// than 2.5 degrees, reported as 0.9% of the sphere; 42 by 32 leave 6.1%, which is pinned so that
// where a frame's edge is read moves it.
TEST(CoverageGaps, FramesNarrowerThanThePlannedLensLeaveGapsBetweenCellsThatFillingCloses) {
  const CapturePlan plan = Plan(Spec());
  const Intrinsics actual = LensOf(42.0, 32.0);
  RingsCoveragePlannerEngine engine;
  auto gaps = engine.FindGaps(plan, AtEveryCell(plan, actual), actual);
  ASSERT_TRUE(gaps.ok()) << gaps.status.detail;
  ASSERT_FALSE(gaps.value.empty());
  EXPECT_NEAR(Fraction(gaps.value), 0.0613, 0.003);

  const std::vector<PlacedFrame> filled =
      Filled(AtEveryCell(plan, actual), gaps.value, actual, 5.0);
  auto after = engine.FindGaps(plan, filled, actual);
  ASSERT_TRUE(after.ok()) << after.status.detail;
  EXPECT_TRUE(after.value.empty()) << after.value.size() << " left";
  EXPECT_EQ(OpenBetweenTheSamples(plan, filled), 0);
}

// And aimed anywhere in the cones, as a user holding still inside one does: the same lens leaves
// gaps where aimed at the centres it left none.
TEST(CoverageGaps, FramesAtTheirConesEdgeOpenGapsTheCentresDidNot) {
  const CapturePlan plan = Plan(Spec());
  const Intrinsics actual = LensOf(50.0, 38.0);
  RingsCoveragePlannerEngine engine;
  auto centred = engine.FindGaps(plan, AtEveryCell(plan, actual), actual);
  ASSERT_TRUE(centred.ok());
  ASSERT_TRUE(centred.value.empty());

  std::vector<PlacedFrame> edged;
  for (size_t n = 0; n < plan.nodes.size(); ++n) {
    const double around = std::numbers::pi / 2.0 * static_cast<double>(n % 4);
    const Quat off = FromAxisAngle(Vec3{std::cos(around), std::sin(around), 0.0},
                                   plan.spec.acceptanceConeDeg / kRadToDeg);
    edged.push_back(PlacedFrame{Multiply(plan.nodes[n].targetOrientation, off), actual});
  }
  auto gaps = engine.FindGaps(plan, edged, actual);
  ASSERT_TRUE(gaps.ok());
  ASSERT_FALSE(gaps.value.empty());
  const std::vector<PlacedFrame> filled = Filled(edged, gaps.value, actual, 5.0);
  auto after = engine.FindGaps(plan, filled, actual);
  ASSERT_TRUE(after.ok());
  EXPECT_TRUE(after.value.empty()) << after.value.size() << " left";
  EXPECT_EQ(OpenBetweenTheSamples(plan, filled), 0);
}

// The lens a review found slivers under: gaps whose nearest asked direction a placed frame covers
// belonged to no target, and filling left them open between the directions asked.
TEST(CoverageGaps, FillingClosesTheGapsBetweenTheDirectionsAskedToo) {
  const CapturePlan plan = Plan(Spec());
  RingsCoveragePlannerEngine engine;
  for (const Intrinsics& actual : {LensOf(42.0, 31.5), LensOf(42.0, 32.0), LensOf(44.5, 33.7)}) {
    auto gaps = engine.FindGaps(plan, AtEveryCell(plan, actual), actual);
    ASSERT_TRUE(gaps.ok()) << gaps.status.detail;
    const std::vector<PlacedFrame> filled =
        Filled(AtEveryCell(plan, actual), gaps.value, actual, 5.0);
    EXPECT_EQ(OpenBetweenTheSamples(plan, filled), 0) << HorizontalFovDeg(actual);
  }
}

TEST(CoverageGaps, NothingPlacedIsTheWholeSphereAndFillingItCloses) {
  const CapturePlan plan = Plan(Spec());
  const Intrinsics lens = LensOf(66.0, 50.0);
  RingsCoveragePlannerEngine engine;
  auto gaps = engine.FindGaps(plan, {}, lens);
  ASSERT_TRUE(gaps.ok()) << gaps.status.detail;
  EXPECT_NEAR(Fraction(gaps.value), 1.0, 1e-9);
  // And in about as many targets as a reach of this lens tiles the sphere with, gathered greedily:
  // asking too few directions per target, as a window half its reach did, leaves more of them.
  EXPECT_NEAR(static_cast<double>(gaps.value.size()), 64.0, 4.0);

  const std::vector<PlacedFrame> filled = Filled({}, gaps.value, lens, 5.0);
  auto after = engine.FindGaps(plan, filled, lens);
  ASSERT_TRUE(after.ok()) << after.status.detail;
  EXPECT_TRUE(after.value.empty()) << after.value.size() << " left";
  EXPECT_EQ(OpenBetweenTheSamples(plan, filled), 0);
}

// A band answers for the elevations its rings aim at, as the plan's own check does (ADR 0076): its
// caps are not a gap.
TEST(CoverageGaps, ABandsCapsAreNotAGap) {
  CapturePlanSpec spec = Spec();
  spec.coverPoles = false;
  const CapturePlan plan = Plan(spec);
  const Intrinsics lens = LensOf(66.0, 50.0);
  RingsCoveragePlannerEngine engine;
  auto gaps = engine.FindGaps(plan, AtEveryCell(plan, lens), lens);
  ASSERT_TRUE(gaps.ok()) << gaps.status.detail;
  EXPECT_TRUE(gaps.value.empty()) << gaps.value.size() << " gaps, " << Fraction(gaps.value);
  // And with nothing placed, only the band is open: the share of the sphere within its highest
  // ring's height either side of the horizon, which is that height.
  auto band = engine.FindGaps(plan, {}, lens);
  ASSERT_TRUE(band.ok());
  double highest = 0.0;
  for (const CoverageNode& node : plan.nodes) {
    highest = std::max(highest, std::abs(Direction(node.targetOrientation).y));
  }
  EXPECT_NEAR(Fraction(band.value), highest, 0.002);
}

// A band with only its horizon ring aims at no elevation but zero, and no point of the spread
// lands there: the plan's own check asks the horizon outright for that reason (ADR 0076), and so
// must this.
TEST(CoverageGaps, ABandOfOnlyItsHorizonRingStillHasAHorizonToCover) {
  CapturePlanSpec spec = Spec(150.0, 120.0);
  spec.coverPoles = false;
  const CapturePlan plan = Plan(spec);
  for (const CoverageNode& node : plan.nodes) {
    ASSERT_NEAR(ElevationOf(node.targetOrientation), 0.0, 1e-9) << "the premise: horizon only";
  }
  RingsCoveragePlannerEngine engine;
  auto gaps = engine.FindGaps(plan, {}, LensOf(150.0, 120.0));
  ASSERT_TRUE(gaps.ok()) << gaps.status.detail;
  EXPECT_FALSE(gaps.value.empty());
  const std::vector<PlacedFrame> filled = Filled({}, gaps.value, LensOf(150.0, 120.0), 5.0);
  auto after = engine.FindGaps(plan, filled, LensOf(150.0, 120.0));
  ASSERT_TRUE(after.ok());
  EXPECT_TRUE(after.value.empty()) << after.value.size() << " left";
  EXPECT_EQ(OpenBetweenTheSamples(plan, filled), 0);
}

// A fill frame that covers less than the directions are apart cannot be told where to go: every
// open direction would be its own target, tens of thousands of them, more than a plan may hold.
TEST(CoverageGaps, AFillLensTooNarrowToResolveIsRefusedRatherThanAnsweredDirectionByDirection) {
  const CapturePlan plan = Plan(Spec());
  RingsCoveragePlannerEngine engine;
  // Half the field, less the 5-degree cone and the 1.25 degrees between directions, against a floor
  // of 1.5: 15.4 degrees across reaches 1.45 and is refused, 15.6 reaches 1.55 and answers.
  EXPECT_EQ(engine.FindGaps(plan, {}, LensOf(15.4, 15.4)).status.code,
            StatusCode::InvalidArgument);
  auto resolved = engine.FindGaps(plan, {}, LensOf(15.6, 15.6));
  EXPECT_TRUE(resolved.ok()) << resolved.status.detail;
}

// A gap's target is a direction, and nothing about the gap says which way up to hold the phone.
TEST(CoverageGaps, ATargetIsAnAimWithNoRoll) {
  const CapturePlan plan = Plan(Spec());
  RingsCoveragePlannerEngine engine;
  auto gaps = engine.FindGaps(plan, {}, LensOf(66.0, 50.0));
  ASSERT_TRUE(gaps.ok());
  for (const CoverageGap& gap : gaps.value) {
    const Vec3 at = Direction(gap.target);
    const double azimuth = std::atan2(-at.x, -at.z) * kRadToDeg;
    const double elevation = std::asin(std::clamp(at.y, -1.0, 1.0)) * kRadToDeg;
    const Quat level = FromAzimuthElevation(azimuth, elevation);
    const double dot = level.w * gap.target.w + level.x * gap.target.x + level.y * gap.target.y +
                       level.z * gap.target.z;
    EXPECT_NEAR(std::abs(dot), 1.0, 1e-9);
  }
}

// Each target is the directions within one fill frame's reach of it, so none says it is more of
// the sphere than that cap; and they share the sphere between them rather than one holding it.
TEST(CoverageGaps, EachTargetIsAtMostOneFillFramesShareOfTheSphere) {
  const CapturePlan plan = Plan(Spec());
  const Intrinsics lens = LensOf(66.0, 50.0);
  RingsCoveragePlannerEngine engine;
  auto gaps = engine.FindGaps(plan, {}, lens);
  ASSERT_TRUE(gaps.ok());
  // Half the narrower field, less the 5-degree cone and the 1.25 degrees between directions.
  const double reach = (25.0 - 5.0 - 1.25) / kRadToDeg;
  const double cap = (1.0 - std::cos(reach)) / 2.0;
  for (const CoverageGap& gap : gaps.value) {
    EXPECT_LE(gap.solidAngleFraction, cap * 1.02 + 1.0 / 20000.0);
  }
}

// The circle a fill frame covers is its nearest edge's, which an off-centre lens moves: half its
// field across overstates it on the near side, and filling then left slivers there.
TEST(CoverageGaps, AnOffCentreFillLensReachesOnlyAsFarAsItsNearestEdge) {
  const CapturePlan plan = Plan(Spec());
  Intrinsics lens = LensOf(66.0, 50.0);
  lens.cx = 0.3 * lens.width;
  ASSERT_TRUE(IsUsableLens(lens));
  RingsCoveragePlannerEngine engine;
  auto gaps = engine.FindGaps(plan, {}, lens);
  ASSERT_TRUE(gaps.ok()) << gaps.status.detail;
  EXPECT_EQ(OpenBetweenTheSamples(plan, Filled({}, gaps.value, lens, 5.0)), 0);
  // Closure alone cannot tell, since fill frames overlap and cover for each other: each target is
  // held to the share of the sphere the near edge's reach covers.
  const UnprojectedDirection near = Unproject(lens, Pixel{0.0, lens.cy});
  ASSERT_TRUE(near.valid);
  const double reach = std::acos(-near.direction.z) - (5.0 + 1.25) / kRadToDeg;
  for (const CoverageGap& gap : gaps.value) {
    EXPECT_LE(gap.solidAngleFraction, (1.0 - std::cos(reach)) / 2.0 * 1.02 + 1.0 / 20000.0);
  }
}

// Placed frames are read through their own distortion, and so is a fill lens's reach.
TEST(CoverageGaps, ADistortedLensIsReadThroughItsDistortion) {
  const CapturePlan plan = Plan(Spec());
  Intrinsics lens = LensOf(44.0, 33.0);
  lens.k1 = -0.15;
  ASSERT_TRUE(IsUsableLens(lens));
  RingsCoveragePlannerEngine engine;
  auto gaps = engine.FindGaps(plan, AtEveryCell(plan, lens), lens);
  ASSERT_TRUE(gaps.ok()) << gaps.status.detail;
  // A barrel lens sees wider than its focal length says, so read as a pinhole it would leave more
  // open than it does; the share is pinned for that reason, since filling closes either way.
  EXPECT_NEAR(Fraction(gaps.value), 0.0154, 0.002);
  const std::vector<PlacedFrame> filled = Filled(AtEveryCell(plan, lens), gaps.value, lens, 5.0);
  EXPECT_EQ(OpenBetweenTheSamples(plan, filled), 0);
}

// A barrel lens strong enough that its corners lie past the fold, so `Unproject` has no direction
// for them: such a frame is asked of its whole half-sphere, and `Project` decides. Asked of none of
// it, every frame would cover nothing and the capture would read as one wide gap. The plan is laid
// for the field this lens has, about 76 by 52, so its frames are needed nearly to their edges: laid
// for 66 by 50 it needed only their middles, and a fallback of 28 degrees passed.
TEST(CoverageGaps, AFrameWhoseCornersDoNotUnprojectIsAskedOfItsHalfSphere) {
  const CapturePlan plan = Plan(Spec(76.0, 52.0));
  Intrinsics barrel = LensOf(66.0, 50.0);
  barrel.k1 = -0.3;
  ASSERT_TRUE(IsUsableLens(barrel));
  ASSERT_FALSE(Unproject(barrel, Pixel{0.0, 0.0}).valid) << "the premise: a corner past the fold";
  RingsCoveragePlannerEngine engine;
  auto gaps = engine.FindGaps(plan, AtEveryCell(plan, barrel), LensOf(66.0, 50.0));
  ASSERT_TRUE(gaps.ok()) << gaps.status.detail;
  EXPECT_TRUE(gaps.value.empty()) << gaps.value.size() << " gaps, " << Fraction(gaps.value);
}

// Whether a direction is imaged by any of the frames, asked with `Project` alone.
bool Imaged(const std::vector<PlacedFrame>& frames, const Vec3& direction) {
  return std::any_of(frames.begin(), frames.end(), [&](const PlacedFrame& frame) {
    const ProjectedPixel at = Project(frame.lens, Rotate(Conjugate(frame.rotation), direction));
    return at.valid && at.pixel.x >= 0.0 && at.pixel.x <= frame.lens.width && at.pixel.y >= 0.0 &&
           at.pixel.y <= frame.lens.height;
  });
}

// A hole `epsDeg` either side of `at` and nothing else near it: four square frames 60 degrees
// across, each aimed past `at` by half its field and `epsDeg`, so their near edges frame a small
// square there; and every cell of the plan more than 70 degrees away, so what else is open is too
// far for a fill frame aimed at it to reach this one.
std::vector<PlacedFrame> AHoleAt(const CapturePlan& plan, const Quat& at, double epsDeg) {
  const Intrinsics square = LensOf(60.0, 60.0);
  std::vector<PlacedFrame> frames;
  for (const Vec3& axis : {Vec3{1.0, 0.0, 0.0}, Vec3{-1.0, 0.0, 0.0}, Vec3{0.0, 1.0, 0.0},
                           Vec3{0.0, -1.0, 0.0}}) {
    frames.push_back(PlacedFrame{Multiply(at, FromAxisAngle(axis, (30.0 + epsDeg) / kRadToDeg)),
                                 square});
  }
  const Vec3 there = Direction(at);
  for (const CoverageNode& node : plan.nodes) {
    if (Dot(Direction(node.targetOrientation), there) < std::cos(70.0 / kRadToDeg)) {
      frames.push_back(PlacedFrame{node.targetOrientation, LensOf(66.0, 50.0)});
    }
  }
  return frames;
}

// Where the spread thins, a hole narrower than its spacing fell between the directions asked,
// each of them covered with its ring: at a band's edge, where the spread is cut, and at a pole,
// where it closes. Both are asked outright now, and filling closes what is open there.
TEST(CoverageGaps, AHoleAtABandsEdgeOrAPoleIsFoundAndFilled) {
  CapturePlanSpec band = Spec();
  band.coverPoles = false;
  struct Case {
    CapturePlan plan;
    double elevation;
    double eps;
  };
  const CapturePlan banded = Plan(band);
  double highest = 0.0;
  for (const CoverageNode& node : banded.nodes) {
    highest = std::max(highest, ElevationOf(node.targetOrientation));
  }
  // At an azimuth where the hole holds none of the directions the engine asks, the band's edge
  // ring included: at 37 degrees it held some, and the test passed on an engine that asked no
  // edges at all.
  for (const Case& c : {Case{banded, highest, 0.05}, Case{Plan(Spec()), 90.0, 0.05}}) {
    const Quat at = FromAzimuthElevation(74.0, c.elevation);
    const std::vector<PlacedFrame> frames = AHoleAt(c.plan, at, c.eps);
    ASSERT_FALSE(Imaged(frames, Direction(at))) << "the premise: a hole there";
    RingsCoveragePlannerEngine engine;
    const Intrinsics lens = LensOf(66.0, 50.0);
    auto gaps = engine.FindGaps(c.plan, frames, lens);
    ASSERT_TRUE(gaps.ok()) << gaps.status.detail;
    EXPECT_TRUE(Imaged(Filled(frames, gaps.value, lens, 5.0), Direction(at))) << c.elevation;
  }
}

// The point of the sphere farthest from every direction the engine asks, about a degree from the
// nearest, is where a hole has the least to be seen by. Rebuilt here as ADR 0076 lays them, and
// searched for rather than reasoned: a hole a tenth of a degree across there is found only if a
// direction near it counts as covered by no one frame, which is what its ring is for. A ring of
// 0.9 degrees misses it; one of four points rather than eight does not, here, and is left to the
// hull argument.
TEST(CoverageGaps, AHoleAtTheFarthestPointFromEveryDirectionAskedIsFound) {
  std::vector<Vec3> asked;
  const double golden = std::numbers::pi * (3.0 - std::sqrt(5.0));
  for (int i = 0; i < 20000; ++i) {
    const double y = 1.0 - 2.0 * (static_cast<double>(i) + 0.5) / 20000.0;
    const double radius = std::sqrt(std::max(0.0, 1.0 - y * y));
    asked.push_back(Vec3{std::cos(golden * i) * radius, y, std::sin(golden * i) * radius});
  }
  // Away from the poles and the horizon, which are asked besides; between 20 and 60 degrees up.
  Vec3 farthest{};
  double worst = 0.0;
  const auto fraction = [](double x) { return x - std::floor(x); };
  for (int k = 0; k < 200000; ++k) {
    const double y = std::sin((20.0 + 40.0 * fraction(k * 0.6180339887498949)) / kRadToDeg);
    const double azimuth = 2.0 * std::numbers::pi * fraction(k * 0.7548776662466927);
    const double radius = std::sqrt(1.0 - y * y);
    const Vec3 at{std::sin(azimuth) * radius, y, -std::cos(azimuth) * radius};
    double nearest = -1.0;
    const int from = std::max(0, static_cast<int>((1.0 - y - 0.03) * 10000.0));
    const int to = std::min(20000, static_cast<int>((1.0 - y + 0.03) * 10000.0) + 1);
    for (int i = from; i < to; ++i) nearest = std::max(nearest, Dot(asked[i], at));
    const double angle = std::acos(std::min(1.0, nearest));
    if (angle > worst) {
      worst = angle;
      farthest = at;
    }
  }
  ASSERT_GT(worst * kRadToDeg, 0.95) << "the premise: a point about a degree from every direction";
  const CapturePlan plan = Plan(Spec());
  const Quat at = FromAzimuthElevation(std::atan2(-farthest.x, -farthest.z) * kRadToDeg,
                                       std::asin(farthest.y) * kRadToDeg);
  RingsCoveragePlannerEngine engine;
  const Intrinsics lens = LensOf(66.0, 50.0);
  for (const double turn : {0.0, 22.5, 45.0, 67.5}) {
    const Quat turned = Multiply(at, FromAxisAngle(Vec3{0.0, 0.0, 1.0}, turn / kRadToDeg));
    const std::vector<PlacedFrame> frames = AHoleAt(plan, turned, 0.05);
    ASSERT_FALSE(Imaged(frames, Direction(at)));
    auto gaps = engine.FindGaps(plan, frames, lens);
    ASSERT_TRUE(gaps.ok());
    EXPECT_TRUE(Imaged(Filled(frames, gaps.value, lens, 5.0), Direction(at))) << turn;
  }
}

TEST(CoverageGaps, RefusesWhatCannotBeJudged) {
  const CapturePlan plan = Plan(Spec());
  const Intrinsics lens = LensOf(66.0, 50.0);
  RingsCoveragePlannerEngine engine;
  // No plan: nothing says which directions are the capture's to cover.
  EXPECT_EQ(engine.FindGaps(CapturePlan{}, {}, lens).status.code, StatusCode::InvalidArgument);
  // No fill lens, and one too narrow for the cone to be aimed anywhere in and still cover: wide
  // enough to cover more than the directions are apart if the cone were not counted, and not
  // once it is.
  EXPECT_EQ(engine.FindGaps(plan, {}, Intrinsics{}).status.code, StatusCode::InvalidArgument);
  // And one whose border folds before its corners: placed, it is asked of its half-sphere; to fill
  // with, its reach cannot be read, and is refused rather than guessed from its midpoints.
  Intrinsics folding = lens;
  folding.k1 = -0.3;
  ASSERT_TRUE(IsUsableLens(folding));
  EXPECT_EQ(engine.FindGaps(plan, {}, folding).status.code, StatusCode::InvalidArgument);
  EXPECT_EQ(engine.FindGaps(plan, {}, LensOf(12.0, 12.0)).status.code,
            StatusCode::InvalidArgument);
  // A cone that is not a measurement, each way it can fail to be one.
  for (const double cone : {0.0, -5.0, std::numeric_limits<double>::quiet_NaN(),
                            std::numeric_limits<double>::infinity()}) {
    CapturePlan unconed = plan;
    unconed.spec.acceptanceConeDeg = cone;
    EXPECT_EQ(engine.FindGaps(unconed, {}, lens).status.code, StatusCode::InvalidArgument) << cone;
  }
  // A frame that is not a placement.
  const std::vector<PlacedFrame> unlensed{PlacedFrame{Quat{}, Intrinsics{}}};
  EXPECT_EQ(engine.FindGaps(plan, unlensed, lens).status.code, StatusCode::InvalidArgument);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const std::vector<PlacedFrame> unturned{PlacedFrame{Quat{nan, 0.0, 0.0, 0.0}, lens}};
  EXPECT_EQ(engine.FindGaps(plan, unturned, lens).status.code, StatusCode::InvalidArgument);
}

TEST(CoverageGaps, TheNullPlannerJudgesNone) {
  NullCoveragePlannerEngine engine;
  EXPECT_EQ(engine.FindGaps(CapturePlan{}, {}, LensOf(66.0, 50.0)).status.code,
            StatusCode::Unsupported);
}

}  // namespace
}  // namespace sphanorama
