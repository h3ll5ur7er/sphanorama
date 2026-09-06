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

  // Which cell should the user go to from here, and how far off are they?
  //
  // Aim first, then coverage. A camera resting inside a cell's acceptance cone is pointing at
  // that cell, and that is the cell to name — captured or not, because a target that moved out
  // from under a still phone is how three presses at one spot filled three cells (ADR 0041,
  // superseding ADR 0027's rule). Outside every cone there is nothing to hold on, so the nearest
  // cell the user still *needs* is the answer and the capture keeps moving.
  //
  // What the coverage state buys is then the *action* rather than the target: `HoldStill` on a
  // cell that wants shooting, `AlreadyCaptured` on one that does not, so nobody is told to
  // photograph what they already have. Coverage arrives as Evaluate's answer rather than as the
  // candidates, so what counts as covered is defined in exactly one place.
  //
  // An empty state means no information rather than nothing missing: at the start of a session
  // nothing has been captured and nothing is a hole, and reading that as a finished sphere would
  // end a capture before it began.
  virtual Result<CaptureGuidance> Locate(const Quat& current, const CapturePlan&,
                                         const CoverageState&) = 0;

  virtual Result<CoverageState> Evaluate(const CapturePlan&, std::span<const Candidate>) = 0;

  // Which cells would most improve coverage if re-shot? Drives retake suggestions.
  virtual Result<std::vector<NodeId>> SuggestRetakes(const CapturePlan&, const CoverageState&,
                                                     const GhostReport&) = 0;
};

}  // namespace sphanorama
