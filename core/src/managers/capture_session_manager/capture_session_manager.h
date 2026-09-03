#pragma once

#include <map>
#include <vector>

#include "sphanorama/engines/coverage_planner_engine.h"
#include "sphanorama/engines/frame_quality_engine.h"
#include "sphanorama/engines/pose_engine.h"
#include "sphanorama/managers/capture_session_manager.h"
#include "sphanorama/resource_access/camera_access.h"
#include "sphanorama/resource_access/frame_store_access.h"
#include "sphanorama/resource_access/motion_sensor_access.h"
#include "sphanorama/resource_access/project_store_access.h"
#include "sphanorama/utilities/clock.h"

namespace sphanorama {

// Owns a live capture session: the plan, the per-cell candidate sets, the current pose.
//
// It decides *when* to ask each engine and never *what* the answer is. It does not decide where a
// reticle sits (V4), what "best" means (V6), or where bytes live (V11) — asking those questions in
// the right order is the entire job, and it is the part most likely to change with the UX.
class CaptureSessionManager final : public ICaptureSessionManager {
 public:
  CaptureSessionManager(ICoveragePlannerEngine& planner, IPoseEngine& pose,
                        IFrameQualityEngine& quality, ICameraAccess& camera,
                        IMotionSensorAccess& sensor, IFrameStoreAccess& frames,
                        IProjectStoreAccess& projects, IClock& clock);

  Result<SessionId> Begin(ProjectId project, const CapturePlanSpec& spec) override;
  Result<CapturePlan> GetPlan() const override;
  Result<CaptureGuidance> OnMotion(std::span<const ImuSample> samples) override;
  Status ArmBurst(NodeId node, const BurstSpec& burst) override;
  Result<FrameVerdict> OfferFrame(NodeId node, const FrameRef& frame,
                                  const PoseSample& pose) override;
  Result<CoverageState> Coverage() const override;
  Result<std::vector<Candidate>> Candidates(NodeId node) const override;
  Status RequestRetake(NodeId node, bool replace) override;
  Status End() override;

 private:
  Status RequireSession() const;
  bool HasNode(NodeId node) const;
  std::vector<Candidate> AllCandidates() const;
  void Discard(std::vector<Candidate>& candidates);

  // Takes at most one frame for the armed burst and folds it into the cell. Reports whether the
  // burst finished on this tick, so OnMotion can say CellDone exactly once.
  Result<bool> AdvanceBurst();
  // Puts the camera back and forgets a burst in progress. Every path out of a burst goes through
  // here — completion, failure, retake, end of session — because a burst that leaves the exposure
  // locked is a viewfinder the user cannot fix by pointing somewhere else.
  void Disarm(bool rollBack);

  ICoveragePlannerEngine& planner_;
  IPoseEngine& pose_;
  IFrameQualityEngine& quality_;
  ICameraAccess& camera_;
  IMotionSensorAccess& sensor_;
  IFrameStoreAccess& frames_;
  IProjectStoreAccess& projects_;
  IClock& clock_;

  bool active_ = false;
  ProjectId project_;
  SessionId session_;
  CapturePlan plan_;
  // The session's pose state. It lives here rather than inside PoseEngine because a
  // manager is the only thing allowed to be stateful (docs/03 §3.3 rule 4, ADR 0016).
  PoseState pose_state_;
  std::map<uint64_t, std::vector<Candidate>> candidates_;

  // The burst in flight, if any. It is session state and it lives here for the same reason the
  // pose does: a manager is the only thing allowed to be stateful (docs/03 §3.3 rule 4).
  bool firing_ = false;
  NodeId burst_node_;
  BurstSpec burst_spec_;
  int32_t burst_taken_ = 0;
  // Where the cell's candidates ended before this burst started, so an abandoned burst can be
  // rolled back to it without touching evidence an earlier burst left there.
  size_t burst_mark_ = 0;
  int64_t last_frame_ns_ = 0;

  uint64_t next_session_ = 1;
  uint64_t next_candidate_ = 1;
};

}  // namespace sphanorama
