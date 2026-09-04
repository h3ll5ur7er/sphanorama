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
                                                          const CapturePlan& plan,
                                                          const CoverageState& coverage) {
  if (plan.nodes.empty()) {
    return Err<CaptureGuidance>(StatusCode::FailedPrecondition, kComponent, "plan has no cells");
  }

  // Nearest cell by the direction the camera looks, and roll reported separately — the same
  // semantics as the real planner, deliberately. This engine is what the manager tests run
  // against, so a guidance rule that differs here makes those tests assert something the shipped
  // path does not do; comparing whole attitudes would fold roll into the aim and recreate the
  // reticle that never closes when the phone is held at an angle.
  //
  // Trivial with one cell, but it is the shape a real tessellation needs, so the manager's
  // sequence does not change when V4 lands.
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
