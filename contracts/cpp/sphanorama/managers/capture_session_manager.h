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

  // Called at sensor rate from the capture loop. Cheap by contract.
  virtual Result<CaptureGuidance> OnMotion(std::span<const ImuSample> samples) = 0;

  // Fires a burst at the given cell and folds the results into its candidate set.
  virtual Result<std::vector<Candidate>> CaptureCell(NodeId node, const BurstSpec& burst) = 0;

  // For externally sourced frames: file import, replayed datasets, manual shutter.
  virtual Result<FrameVerdict> OfferFrame(NodeId node, const FrameRef& frame,
                                          const PoseSample& pose) = 0;

  virtual Result<CoverageState> Coverage() const = 0;
  virtual Result<std::vector<Candidate>> Candidates(NodeId node) const = 0;

  // Re-arms a cell. Existing candidates are kept unless `replace` is set, so a retake can add to
  // the evidence pool rather than discard it.
  virtual Status RequestRetake(NodeId node, bool replace) = 0;

  virtual Status End() = 0;
};

}  // namespace sphanorama
