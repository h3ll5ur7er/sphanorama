#pragma once

#include "sphanorama/engines/coverage_planner_engine.h"

namespace sphanorama {

// V4, as horizontal rings.
//
// Rings rather than a geodesic tessellation because a capture is performed by a person turning
// on the spot: sweeping a ring and stepping up is a motion someone can follow, and a geodesic
// layout's cells arrive in an order that reads as random from behind the phone. The cost is a few
// more cells near the poles than a geodesic layout would need, which is a trade worth making
// until someone measures otherwise.
//
// Azimuth spacing widens with elevation by 1/cos(elevation), because a ring's circumference
// shrinks as it climbs; spacing it uniformly in azimuth would put hundreds of redundant cells at
// the poles and leave the equator short.
class RingsCoveragePlannerEngine final : public ICoveragePlannerEngine {
 public:
  Result<CapturePlan> Plan(const CapturePlanSpec& spec, const Intrinsics& lens) override;
  Result<CaptureGuidance> Locate(const Quat& current, const CapturePlan& plan) override;
  Result<CoverageState> Evaluate(const CapturePlan& plan,
                                 std::span<const Candidate> candidates) override;
  Result<std::vector<NodeId>> SuggestRetakes(const CapturePlan& plan, const CoverageState& state,
                                             const GhostReport& ghosts) override;
};

}  // namespace sphanorama
