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
  // Takes the coverage state because "nearest" is not the question a person is asking — they want
  // the nearest cell they still *need*. Answering with the nearest cell of any kind aims someone
  // at a cell they have already captured and tells them to hold still, which from behind a phone
  // is indistinguishable from working. Coverage arrives as Evaluate's answer rather than as the
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
