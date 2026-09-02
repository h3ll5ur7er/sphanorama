#include "engines/coverage_planner_engine/null_coverage_planner_engine.h"

#include <algorithm>

#include "utilities/quaternion.h"

namespace sphanorama {
namespace {
constexpr const char* kComponent = "NullCoveragePlannerEngine";
constexpr double kRadToDeg = 57.29577951308232;
}  // namespace

Result<CapturePlan> NullCoveragePlannerEngine::Plan(const CapturePlanSpec& spec,
                                                    const Intrinsics&) {
  if (spec.acceptanceConeDeg <= 0.0) {
    return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                            "acceptance cone must be positive");
  }

  CapturePlan plan;
  plan.spec = spec;
  CoverageNode node;
  node.id = NodeId{1};
  node.targetOrientation = Quat{};   // identity: straight ahead
  node.acceptanceConeDeg = spec.acceptanceConeDeg;
  plan.nodes.push_back(node);
  return Ok(std::move(plan));
}

Result<CaptureGuidance> NullCoveragePlannerEngine::Locate(const Quat& current,
                                                          const CapturePlan& plan) {
  if (plan.nodes.empty()) {
    return Err<CaptureGuidance>(StatusCode::FailedPrecondition, kComponent, "plan has no cells");
  }

  // Nearest cell by angular distance. Trivial with one cell, but it is the shape a real
  // tessellation needs, so the manager's sequence does not change when V4 lands.
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

Result<CoverageState> NullCoveragePlannerEngine::Evaluate(const CapturePlan& plan,
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

Result<std::vector<NodeId>> NullCoveragePlannerEngine::SuggestRetakes(const CapturePlan&,
                                                                      const CoverageState& state,
                                                                      const GhostReport& ghosts) {
  // Holes first, then anything ghosted: both are cells a user would want to re-shoot, and it is
  // the order a real planner would start from too.
  std::vector<NodeId> suggestions = state.holes;
  for (const auto& region : ghosts.regions) {
    const bool already = std::any_of(suggestions.begin(), suggestions.end(),
                                     [&](NodeId id) { return id.value == region.node.value; });
    if (!already) suggestions.push_back(region.node);
  }
  return Ok(std::move(suggestions));
}

}  // namespace sphanorama
