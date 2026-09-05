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
  virtual Status ArmBurst(NodeId node, const BurstSpec& burst) = 0;

  // For externally sourced frames: file import, replayed datasets, manual shutter.
  virtual Result<FrameVerdict> OfferFrame(NodeId node, const FrameRef& frame,
                                          const PoseSample& pose) = 0;

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
  // has after it.
  virtual Result<FramePreview> CandidatePreview(NodeId node, CandidateId candidate,
                                                int32_t maxEdge) const = 0;

  // Re-arms a cell. Existing candidates are kept unless `replace` is set, so a retake can add to
  // the evidence pool rather than discard it.
  virtual Status RequestRetake(NodeId node, bool replace) = 0;

  virtual Status End() = 0;
};

}  // namespace sphanorama
