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

  // Re-arms a cell. Existing candidates are kept unless `replace` is set, so a retake can add to
  // the evidence pool rather than discard it.
  virtual Status RequestRetake(NodeId node, bool replace) = 0;

  virtual Status End() = 0;
};

}  // namespace sphanorama
