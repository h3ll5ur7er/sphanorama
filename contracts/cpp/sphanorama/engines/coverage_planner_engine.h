#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V4 — how the sphere is tessellated and coverage is judged. Stateless: every call carries the
// plan it operates on, so a session's state stays in the manager that owns the sequence.
class ICoveragePlannerEngine {
 public:
  virtual ~ICoveragePlannerEngine() = default;

  virtual Result<CapturePlan> Plan(const CapturePlanSpec&, const Intrinsics& lens) = 0;

  // Which cell is this orientation aiming at, and how far off is it?
  virtual Result<CaptureGuidance> Locate(const Quat& current, const CapturePlan&) = 0;

  virtual Result<CoverageState> Evaluate(const CapturePlan&, std::span<const Candidate>) = 0;

  // Which cells would most improve coverage if re-shot? Drives retake suggestions.
  virtual Result<std::vector<NodeId>> SuggestRetakes(const CapturePlan&, const CoverageState&,
                                                     const GhostReport&) = 0;
};

}  // namespace sphanorama
