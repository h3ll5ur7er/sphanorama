#include "engines/coverage_planner_engine/rings_coverage_planner_engine.h"

#include <algorithm>
#include <cmath>

#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr const char* kComponent = "RingsCoveragePlannerEngine";
constexpr double kRadToDeg = 57.29577951308232;
constexpr double kDegToRad = 0.017453292519943295;

// Below this the 1/cos(elevation) widening runs away, so a ring that close to a pole is served by
// a single cell — which is correct: at the pole every azimuth looks at the same place.
constexpr double kPolarCosineFloor = 0.08;

}  // namespace

Result<CapturePlan> RingsCoveragePlannerEngine::Plan(const CapturePlanSpec& spec,
                                                     const Intrinsics&) {
  if (spec.strategy != TessellationStrategy::Rings) {
    // Refused rather than substituted. The plan carries the spec back to the caller, so quietly
    // laying rings for a geodesic request would have the plan claim a layout it does not have,
    // and anything reasoning about cell spacing from the strategy it asked for would be reasoning
    // about a different sphere.
    return Err<CapturePlan>(StatusCode::Unsupported, kComponent,
                            "this engine tessellates in rings; no other strategy is implemented");
  }

  // Finiteness first. The spec crosses the facade as doubles, where NaN and Infinity are ordinary
  // values a client can send, and both slip past a ">= 0" test — then reach the conversions
  // below, where a float outside int's range is undefined behaviour rather than a large number.
  if (!std::isfinite(spec.horizontalFovDeg) || !std::isfinite(spec.verticalFovDeg) ||
      !std::isfinite(spec.overlapTarget) || !std::isfinite(spec.acceptanceConeDeg)) {
    return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                            "the plan spec contains a value that is not a finite number");
  }
  if (spec.horizontalFovDeg <= 0.0 || spec.verticalFovDeg <= 0.0) {
    return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                            "the lens field of view is unknown; nothing can be tessellated");
  }
  if (spec.horizontalFovDeg > 180.0 || spec.verticalFovDeg > 180.0) {
    return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                            "a field of view wider than 180 degrees is not a lens");
  }
  if (spec.overlapTarget < 0.0 || spec.overlapTarget >= 1.0) {
    // An overlap of 1 is a step of zero degrees: infinitely many cells.
    return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                            "overlap must be at least 0 and less than 1");
  }
  if (spec.acceptanceConeDeg <= 0.0) {
    return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                            "acceptance cone must be positive");
  }

  const double advance = 1.0 - spec.overlapTarget;
  const double verticalStep = spec.verticalFovDeg * advance;
  const double horizontalStep = spec.horizontalFovDeg * advance;

  // A lens narrow enough to need more cells than this is not a capture anyone completes — at one
  // second a cell it is over a day of holding a phone still. The bound exists so the counts below
  // stay inside int, since converting an out-of-range double to int is undefined behaviour and
  // arrives here as a plan rather than as a diagnosable failure.
  constexpr double kMaxCells = 100000.0;
  const double approximateRings = 180.0 / verticalStep + 1.0;
  const double approximateCells = approximateRings * (360.0 / horizontalStep + 1.0);
  if (!std::isfinite(approximateCells) || approximateCells > kMaxCells) {
    return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                            "this lens and overlap would need more cells than a session can hold");
  }

  // Rings are laid symmetrically about the horizon and reach the poles, so that a sphere is
  // covered rather than a band. An odd count keeps one ring exactly on the horizon, which is
  // where most of the interesting content is and where the user starts.
  const int halfRings = static_cast<int>(std::ceil(90.0 / verticalStep));
  const int ringCount = 2 * halfRings + 1;
  const double elevationStep = 180.0 / (ringCount - 1);

  CapturePlan plan;
  plan.spec = spec;
  uint64_t nextId = 1;

  // Without the caps the plan covers a band around the horizon instead of a sphere. The limit is
  // the highest elevation a cell can aim at and still have its whole field of view below the
  // pole — dropping every ring above that, rather than just the two pole cells, is what makes
  // "no caps" mean a band rather than a sphere with two holes in it.
  const double elevationLimit = spec.coverPoles ? 90.0 : 90.0 - spec.verticalFovDeg / 2.0;

  for (int ring = 0; ring < ringCount; ++ring) {
    const double elevation = -90.0 + elevationStep * ring;
    if (std::abs(elevation) > elevationLimit) continue;

    // A ring's circumference shrinks by cos(elevation), so the azimuth step widens to keep the
    // on-sphere spacing constant. Without this the poles collect hundreds of redundant cells.
    const double cosine = std::max(std::cos(elevation * kDegToRad), kPolarCosineFloor);
    const int cells = std::max(1, static_cast<int>(std::ceil(360.0 * cosine / horizontalStep)));

    for (int cell = 0; cell < cells; ++cell) {
      CoverageNode node;
      node.id = NodeId{nextId++};
      node.targetOrientation =
          FromAzimuthElevation(360.0 * cell / cells, elevation);
      node.acceptanceConeDeg = spec.acceptanceConeDeg;
      node.ringIndex = ring;
      plan.nodes.push_back(node);
    }
  }

  return Ok(std::move(plan));
}

Result<CaptureGuidance> RingsCoveragePlannerEngine::Locate(const Quat& current,
                                                            const CapturePlan& plan,
                                                            const CoverageState& coverage) {
  if (plan.nodes.empty()) {
    return Err<CaptureGuidance>(StatusCode::FailedPrecondition, kComponent, "plan has no cells");
  }

  // Compared as directions, not as attitudes. AngleBetween on two orientations folds rotation
  // about the optical axis into the answer, so a phone aimed exactly at a cell but held at an
  // angle would read as far off target and the reticle would never close. How the phone is held
  // is a separate correction, and CaptureGuidance has a separate field for it.
  const Vec3 looking = Direction(current);

  // Coverage has an opinion only once something has been evaluated. An empty state is no
  // information rather than nothing missing: at the start of a session nothing is captured and
  // nothing is a hole, and reading that as a finished sphere would end a capture before it began.
  const bool informed = coverage.nodesTotal > 0;
  const auto missing = [&](NodeId id) {
    return std::any_of(coverage.holes.begin(), coverage.holes.end(),
                       [id](NodeId hole) { return hole.value == id.value; });
  };

  double best = 0.0;
  const auto nearestOf = [&](bool onlyMissing) -> const CoverageNode* {
    const CoverageNode* found = nullptr;
    for (const auto& node : plan.nodes) {
      if (onlyMissing && !missing(node.id)) continue;
      const double angle = AngleBetweenDirections(looking, Direction(node.targetOrientation));
      if (found == nullptr || angle < best) {
        best = angle;
        found = &node;
      }
    }
    return found;
  };

  // Only what is still needed, when that is known. Falling back to the whole plan is not merely
  // defensive: a holes list naming cells this plan does not contain would otherwise leave nothing
  // to aim at, and an odd target beats refusing to guide at all.
  const CoverageNode* nearest = informed ? nearestOf(true) : nullptr;
  const bool nothingMissing = informed && nearest == nullptr;
  if (nearest == nullptr) nearest = nearestOf(false);

  CaptureGuidance guidance;
  guidance.targetNode = nearest->id;
  guidance.angularErrorDeg = best * kRadToDeg;
  guidance.rollErrorDeg = RollBetween(current, nearest->targetOrientation) * kRadToDeg;
  // A finished sphere still names a cell and an error, because the fields are read either way —
  // but it says so, which nothing in this engine ever did before, so a completed capture went on
  // asking for whichever cell the phone happened to be nearest.
  guidance.action = nothingMissing ? GuidanceAction::SphereDone
                    : guidance.angularErrorDeg <= nearest->acceptanceConeDeg
                        ? GuidanceAction::HoldStill
                        : GuidanceAction::Seek;
  return Ok(guidance);
}

Result<CoverageState> RingsCoveragePlannerEngine::Evaluate(const CapturePlan& plan,
                                                            std::span<const Candidate> candidates) {
  CoverageState state;
  state.nodesTotal = static_cast<int32_t>(plan.nodes.size());

  for (const auto& node : plan.nodes) {
    const bool covered =
        std::any_of(candidates.begin(), candidates.end(),
                    [&](const Candidate& c) { return c.node.value == node.id.value; });
    if (covered) {
      ++state.nodesSatisfied;
    } else {
      state.holes.push_back(node.id);
    }
  }
  state.coveredSolidAngleFraction =
      state.nodesTotal == 0 ? 0.0
                            : static_cast<double>(state.nodesSatisfied) / state.nodesTotal;
  return Ok(std::move(state));
}

Result<std::vector<NodeId>> RingsCoveragePlannerEngine::SuggestRetakes(const CapturePlan&,
                                                                       const CoverageState& state,
                                                                       const GhostReport& ghosts) {
  // Holes first, then anything ghosted: a cell with no frames at all is a worse problem than one
  // whose frames disagree, and a cell that is both should only be suggested once.
  std::vector<NodeId> suggestions = state.holes;
  for (const auto& region : ghosts.regions) {
    const bool already = std::any_of(suggestions.begin(), suggestions.end(),
                                     [&](NodeId id) { return id.value == region.node.value; });
    if (!already) suggestions.push_back(region.node);
  }
  return Ok(std::move(suggestions));
}

}  // namespace sphanorama
