#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V1 — how a capture session is sequenced. Holds the live session: the plan, the per-cell
// candidate sets, the current pose estimate.
//
// It decides *when* to ask each engine, never *how*: it does not decide what "best" means (V6),
// where a reticle sits (V4), or how bytes are stored (V11).
// @boundary @facade
class ICaptureSessionManager {
 public:
  virtual ~ICaptureSessionManager() = default;

  // Opens a capture session on a project that already exists.
  //
  // Refused with `SensorUnavailable` when `IMotionSensorAccess::Capabilities()` reports `None` or
  // cannot answer, and refused *before* a camera is opened. A capture places every frame by the
  // direction the phone was pointing, and a device that cannot sense one produces cells labelled
  // with directions nobody measured — a failure invisible until a build, so it is refused at the
  // door instead (ADR 0044). A client whose own platform can answer that question earlier should:
  // this call is the rule, not the only place to be polite about it.
  //
  // Refused with `NotFound` when the project does not exist, which is checked first: beginning
  // against an id nobody created would leave a titleless project in the user's list.
  virtual Result<SessionId> Begin(ProjectId project, const CapturePlanSpec& spec) = 0;
  // Picks a session back up from what was written down about it.
  //
  // A tab that goes away mid-capture takes the plan, the candidate sets and the frame store with
  // it, and the phone that comes back is the same phone standing in the same spot — so the cells
  // already captured have to still count. What survives is the project store's documents and
  // whatever the frame store's sink is holding; this reads the first and hands the frames it
  // names back to the store, which is why a resumed candidate can still be pinned.
  //
  // The plan is the stored one rather than a fresh tessellation. Node ids are indices into a
  // particular sphere, so replanning from whatever lens is in front of the phone now would file
  // every restored candidate under a different cell.
  //
  // The motion capability is not stored, and this reads the live one: a document says which
  // sphere is being captured, never what the device it comes back on can sense. So this is
  // refused with `SensorUnavailable` on exactly the terms `Begin` is, and on the same phone that
  // began the capture if the user declined the permission this time (ADR 0044).
  virtual Result<SessionId> Resume(ProjectId project) = 0;

  virtual Result<CapturePlan> GetPlan() const = 0;

  // The session's tick, called at sensor rate from the capture loop. Cheap by contract.
  //
  // It also advances an armed burst by at most one frame, because this is the only call the
  // client makes often enough to pace one: a burst takes time, and time is something a
  // synchronous port cannot wait for (ADR 0018). Guidance reports `Firing` until the burst is
  // full and `CellDone` on the tick that fills it.
  virtual Result<CaptureGuidance> OnMotion(std::span<const ImuSample> samples) = 0;

  // Arms a burst at the given cell. It does not fire one: the frames arrive over the following
  // ticks, and the candidates are readable through Candidates(node) once guidance says CellDone.
  //
  // `burst.intervalMs` is a floor rather than a cadence — at most one frame is taken per tick, so
  // a spec asking for less than a tick apart gets a tick apart, and a spec asking for less than
  // the camera's own `maxBurstFps` period gets that instead. Locks are applied here and held
  // until the burst completes or is abandoned.
  //
  // The first frame is not taken until `burst.settleMs` after arming, because the locks applied
  // on this call are what the camera has to converge to. Under that floor the camera's own frame
  // period applies as well: `PeekPreviewFrame` borrows the latest preview frame, and inside one
  // frame period the latest frame is one the camera produced before the locks landed.
  //
  // Refused with `FailedPrecondition` when the camera is not aimed at the cell — outside the
  // acceptance cone the plan gave it, which is the cone guidance closes its reticle on. A
  // burst records whatever the camera is looking at and the node is only a name to file it under,
  // so arming against a cell somewhere else stores a good picture in the wrong place: sharp, well
  // scored, and undetectable afterwards (ADR 0041). The caller fixes it by turning the phone.
  //
  // Refused with `FailedPrecondition` again, and for a different reason, when that cone is not a
  // measurement — not finite, or not greater than zero. The detail says which: "not a usable
  // measurement" is a broken plan and nothing the user can do anything about, where "not aimed at
  // that cell" is a phone to turn. `ICoveragePlannerEngine::Plan` forbids such a cone and both
  // shipped engines refuse it, so this is the manager declining to assume every implementation of
  // that contract validates its own output — the two that exist disagreed about exactly this,
  // twice, in consecutive rounds of one review.
  //
  // **Unconditionally, and this paragraph used to say the opposite.** Until ADR 0044 the check
  // stood down whenever `PoseSample.confidence` was zero, so that a phone with no motion sensor
  // could reach every cell of its own plan by eye. Such a phone is now refused at `Begin`, and
  // what is left of zero confidence is transient: the ticks before a session's first reading, and
  // a stream carrying rates with no attitude in them. In both the pose is the identity it was
  // born with, which is a direction nobody chose, so there is nothing to check and nothing to
  // allow — arming then would file real pixels under a cell picked by an accident of
  // initialisation.
  //
  // So a client may wait for guidance to say `HoldStill`, which means "inside this cell's cone
  // and this cell still wants shooting" — the condition this arms on, from the same plan and the
  // same cone. It is one condition read twice rather than one call trusting another, so the two
  // can be made to disagree by a plan neither of them wrote: a cone that is not a measurement had
  // `Locate` reporting `HoldStill` while this call refused, which since ADR 0043 is a dwell that
  // matures, fires, is refused, and starts again. `ICoveragePlannerEngine` now states the rule
  // that keeps them together, on `Plan`, where a planner will read it. What it must not
  // assume is that the action always comes: it needs an anchored pose, so a stream that carries
  // rates and never an attitude produces a session that begins and can never arm. The shipped
  // browser adapter cannot produce one (every sample it emits carries an attitude), and a port
  // that can owes its user a way to say so. Nothing in this build watches for it; the roadmap
  // carries it.
  virtual Status ArmBurst(NodeId node, const BurstSpec& burst) = 0;

  // For externally sourced frames: file import, replayed datasets, manual shutter.
  virtual Result<FrameVerdict> OfferFrame(NodeId node, const FrameRef& frame,
                                          const PoseSample& pose) = 0;

  // What the camera this session is using reports it can do, as the manager last read it — which
  // is at `Begin`/`Resume` and again at every `ArmBurst` (ADR 0045). Where that read said nothing,
  // what it last *heard*: a metric of zero is the contract's "the platform will not say", so the
  // rate and the frame's geometry survive a refresh that answers with neither.
  //
  // Here rather than on a port because a port is not on the boundary: `ICameraAccess` is the
  // core's, and the page's own adapter is a different object that happens to answer the same
  // questions. Two answers to one question is what this call exists to stop being possible to
  // ignore — the two sides of the browser seam agree by an integer index and a property name, and
  // nothing checked either. `maxBurstFps` was in `CameraCapabilities` for the life of the field,
  // had no case in the port's metric switch, and read as zero — which the manager is right to
  // treat as "the platform will not say", so a floor that was never wired looked exactly like a
  // browser declining to answer. Nothing failed. A test that reads these back through the facade
  // is what makes a missing case fail instead (ADR 0045).
  //
  // "As the manager last read it", not "as the camera is now": this reports the copy the session
  // is actually pacing bursts by, which is refreshed at `ArmBurst`. A client wanting a status row
  // to be exactly current would be asking the wrong object — that is a fact about the device, and
  // the page holds the device.
  //
  // Refused with `FailedPrecondition` when no session is open, since there is no camera to
  // describe.
  virtual Result<CameraCapabilities> CameraInUse() const = 0;

  virtual Result<CoverageState> Coverage() const = 0;
  // Ranked best-first, by the same `IFrameQualityEngine::Rank` the manager already asks on every
  // committed burst. The order is an answer rather than a record of when the shutter fired, so a
  // review client can show a strip and name the automatic pick without deciding what "best"
  // means — which is V6's, and not a client's to borrow.
  virtual Result<std::vector<Candidate>> Candidates(NodeId node) const = 0;

  // One candidate's frame, reduced to something a screen can take.
  //
  // The counterpart of `Candidates`, and the reason it is here rather than anywhere else: that
  // call hands back what the core *knows* about a candidate, and a person choosing between five
  // frames of the same wall needs to see them. A `FrameRef` cannot do that job — the page has no
  // frame store to resolve one against, and `IFrameStoreAccess::Pin` reaches no further than the
  // core — so this is the one call in the contracts that answers with pixels (ADR 0038).
  //
  // `maxEdge` bounds the long edge and the caller states it, because how large a thumbnail wants
  // to be is a fact about the screen it is going on. It is bounded in turn: past
  // `kFramePreviewMaxEdge` the reduction stops paying for itself and the call is refused.
  //
  // `NotFound` covers both halves of a stale request — a cell that is not in the plan, and a
  // candidate this cell no longer holds. A replace-retake forgets a cell's frames, so a client
  // showing a strip it fetched a moment ago can ask about a candidate that has since gone; that
  // is an ordinary answer here rather than a fault.
  //
  // Reading a preview does not warm a cell. A captured cell's frames have been cooled to whatever
  // cheaper tier the store has (ADR 0023) and faulting one in to look at it would leave it
  // resident — eight candidates of a 1280x960 frame are 39 MB, so a user opening three cells
  // would fill a phone's heap by browsing. Whatever residency a frame had before this call, it
  // has after it: the tier is read first and restored by name, so a store with tiers this build
  // has never seen gets its frame back where it had it. Best effort, and deliberately so — a
  // store that will not take the frame back leaves it readable in the heap, which is a worse
  // ceiling rather than a lost frame.
  virtual Result<FramePreview> CandidatePreview(NodeId node, CandidateId candidate,
                                                int32_t maxEdge) const = 0;

  // Re-arms a cell. Existing candidates are kept unless `replace` is set, so a retake can add to
  // the evidence pool rather than discard it.
  //
  // "Re-arms" is about the cell's state, not about a burst: the burst that follows still goes
  // through `ArmBurst` and is still refused if the camera is not aimed at the cell (ADR 0041). So
  // a retake asks the user to point at the cell again before anything is recorded — which is the
  // point, since a retake that captured from wherever the phone happened to be pointing is the bug
  // ADR 0041 exists to stop. `docs/03-architecture.md` UC-2 describes the flow.
  //
  // **Without exemption, and this paragraph used to carry one.** It said the burst after a retake
  // arms wherever the phone is pointing when the pose was never anchored, because that was the
  // only way a sensorless device could retake at all. That device is refused at `Begin` now
  // (ADR 0044), so the rule above is the whole rule: point at the cell again, and the burst is
  // taken there or not at all.
  //
  // With `replace` false, that burst has nothing to fire it in this build, and the honest place
  // to say so is here. Keeping the evidence leaves the cell covered, `Locate` answers
  // `AlreadyCaptured` rather than `HoldStill` for a covered cell, and the dwell that arms every
  // burst since ADR 0043 only matures on `HoldStill` — so an additive retake marks nothing a
  // client can act on. `replace` true empties the cell of everything the store will let go of,
  // which makes it a hole again and puts it back in the dwell's way — everything, unless the
  // store refuses to forget a frame, in which case that one candidate stays and the cell stays
  // covered until a later retake succeeds. `Discard` keeps it deliberately: the bytes are still
  // charged, and dropping the last handle to them would orphan them. The retake flow that closes
  // the additive case is Phase 3 (`docs/06-roadmap.md`); until then this call aborts a burst in
  // flight and, additively, does nothing else.
  virtual Status RequestRetake(NodeId node, bool replace) = 0;

  virtual Status End() = 0;
};

}  // namespace sphanorama
