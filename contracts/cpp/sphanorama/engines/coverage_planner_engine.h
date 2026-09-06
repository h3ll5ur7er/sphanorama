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
  // cell the user still *needs* is the answer and the capture keeps moving — and where coverage
  // has no opinion at all, the nearest cell of any kind, because "needed" has no meaning yet.
  // That third branch is reached two ways: an uninformed coverage state, which `OnMotion` cannot
  // produce because it evaluates coverage first; and a state with nothing missing, which is every
  // tick of a finished sphere.
  //
  // It is *not* what names the cell in every `SphereDone`, and this sentence has now been wrong
  // three times in three different ways. Aim still comes first on a finished sphere: a phone
  // resting inside a cone gets that cell, and only a phone resting inside none of them falls
  // through to "the nearest cell of any kind". The two answers coincide while every node carries
  // the same `acceptanceConeDeg`, which is true of both `Plan` implementations and is not a
  // property of `CoverageNode` — with a 1° cone on one node and 10° on another they part company.
  //
  // What the coverage state buys is then the *action* rather than the target: `HoldStill` on a
  // cell that wants shooting, `AlreadyCaptured` on one that does not, so nobody is told to
  // photograph what they already have. Coverage arrives as Evaluate's answer rather than as the
  // candidates, so what counts as covered is defined in exactly one place.
  //
  // **A whole `PoseSample` rather than the orientation alone, because the rule above is only
  // valid when there is an aim.** `confidence` is zero when no reading has ever anchored the
  // orientation, which leaves it at the identity it was born with — a direction nobody chose — so
  // preferring the cell "under the camera" would mean preferring whichever cell happens to sit
  // there. With no aim there is nothing to be inside of: no node is named as held, no `HoldStill`
  // is reported, and the target falls back to the nearest cell still missing (ADR 0027's rule).
  //
  // This used to describe a *device* — a phone with no motion sensor, which reported identity for
  // the life of a session and captured by eye (ADR 0042). It now describes a *moment*: such a
  // device is refused at `ICaptureSessionManager::Begin` (ADR 0044), and what is left is a
  // session's opening ticks, before its first reading arrives, and a stream carrying angular
  // rates with no attitude in them, which never anchors at all. An engine that ignored
  // `confidence` would let the second of those mature a dwell and fire a burst at a cell nobody
  // pointed at, which is the failure ADR 0041 exists to stop.
  //
  // An empty state means no information rather than nothing missing: at the start of a session
  // nothing has been captured and nothing is a hole, and reading that as a finished sphere would
  // end a capture before it began.
  virtual Result<CaptureGuidance> Locate(const PoseSample& current, const CapturePlan&,
                                         const CoverageState&) = 0;

  virtual Result<CoverageState> Evaluate(const CapturePlan&, std::span<const Candidate>) = 0;

  // Which cells would most improve coverage if re-shot? Drives retake suggestions.
  virtual Result<std::vector<NodeId>> SuggestRetakes(const CapturePlan&, const CoverageState&,
                                                     const GhostReport&) = 0;
};

}  // namespace sphanorama
