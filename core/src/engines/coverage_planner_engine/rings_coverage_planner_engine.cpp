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
  if (spec.horizontalFovDeg <= 0.0 || spec.verticalFovDeg <= 0.0) {
    return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                            "the lens field of view is unknown; nothing can be tessellated");
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

  // Rings are laid symmetrically about the horizon and reach the poles, so that a sphere is
  // covered rather than a band. An odd count keeps one ring exactly on the horizon, which is
  // where most of the interesting content is and where the user starts.
  const int halfRings = static_cast<int>(std::ceil(90.0 / verticalStep));
  const int ringCount = 2 * halfRings + 1;
  const double elevationStep = 180.0 / (ringCount - 1);

  CapturePlan plan;
  plan.spec = spec;
  uint64_t nextId = 1;

  for (int ring = 0; ring < ringCount; ++ring) {
    const double elevation = -90.0 + elevationStep * ring;

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
                                                            const CapturePlan& plan) {
  if (plan.nodes.empty()) {
    return Err<CaptureGuidance>(StatusCode::FailedPrecondition, kComponent, "plan has no cells");
  }

  const CoverageNode* nearest = &plan.nodes.front();
  double best = AngleBetween(current, nearest->targetOrientation);
  for (const auto& node : plan.nodes) {
    const double angle = AngleBetween(current, node.targetOrientation);
    if (angle < best) {
      best = angle;
      nearest = &node;
    }
  }

  CaptureGuidance guidance;
  guidance.targetNode = nearest->id;
  guidance.angularErrorDeg = best * kRadToDeg;
  guidance.action = guidance.angularErrorDeg <= nearest->acceptanceConeDeg
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
