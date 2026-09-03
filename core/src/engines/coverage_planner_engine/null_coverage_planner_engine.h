#pragma once

#include "sphanorama/engines/coverage_planner_engine.h"

namespace sphanorama {

// Null object for V4. Plans a single cell at identity rather than tessellating the sphere:
// enough for the capture sequence to have a target, obviously not a coverage plan.
//
// "Null" here means minimal but honest. An engine that invented a plausible-looking tessellation
// would let the walking skeleton pass for a working product, which is the one thing it must not do.
class NullCoveragePlannerEngine final : public ICoveragePlannerEngine {
 public:
  Result<CapturePlan> Plan(const CapturePlanSpec& spec, const Intrinsics& lens) override;
  Result<CaptureGuidance> Locate(const Quat& current, const CapturePlan& plan) override;
  Result<CoverageState> Evaluate(const CapturePlan& plan,
                                 std::span<const Candidate> candidates) override;
  Result<std::vector<NodeId>> SuggestRetakes(const CapturePlan& plan, const CoverageState& state,
                                             const GhostReport& ghosts) override;
};

}  // namespace sphanorama
