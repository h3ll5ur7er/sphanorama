#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V4 — how the sphere is tessellated and coverage is judged. Stateless: every call carries the
// plan it operates on, so a session's state stays in the manager that owns the sequence.
class ICoveragePlannerEngine {
 public:
  virtual ~ICoveragePlannerEngine() = default;

  // Tessellates the sphere for this lens.
  //
  // **`spec.acceptanceConeDeg` must be finite and greater than zero, and an implementation that
  // cannot make it so refuses with `InvalidArgument` rather than planning.** Written here because
  // it was written nowhere: it lived inside two implementations, which disagreed about it twice in
  // consecutive rounds of one review — first about `NaN`, then about `+Infinity` — and each time
  // the fix went into the implementation that had been caught.
  //
  // The cone is not a preference. It is the number `Locate` decides "the user is inside this cell"
  // with and the number `ArmBurst` refuses a burst by, so a value that is not a measurement is not
  // a wide cone or a narrow one — it is a plan in which those two calls cannot agree. `inf` loses
  // every `angle > cone`, which reads as *every* direction being inside *every* cell; `NaN` loses
  // both comparisons, so one caller reads it as inside and another as outside; zero and negatives
  // put the camera outside a cell it is pointed exactly at.
  //
  // Both callers guard it anyway, which is not redundancy but the arrangement `Locate` describes
  // below — one guard for what the user sees, one for what the frames depend on.
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
  // rates with no attitude in them, which never anchors at all.
  //
  // An engine that ignored `confidence` would name a cell as held on an orientation nobody chose.
  // `ICaptureSessionManager::ArmBurst` refuses such a burst on its own account, so the reticle
  // would close on a cell that then would not arm — a capture that looks ready and does nothing,
  // which is worse to diagnose than one that says it is seeking. Two guards for one fact is the
  // arrangement here on purpose: this one is what the user sees, and the manager's is what the
  // frames depend on.
  //
  // Two guards only work while they agree, and the same applies to the acceptance cone: **a cell
  // whose cone is not a finite positive number is one no camera is inside**, which is the answer
  // `ArmBurst` gives. They disagreed about it for one round, and the cost was not a wrong reticle:
  // guidance said `HoldStill` on a cell ninety degrees away, the dwell matured, `Fire` went out,
  // `ArmBurst` refused the same cone as unusable, the dwell restarted, and it repeated — with no
  // shutter left to escape it since ADR 0044.
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
