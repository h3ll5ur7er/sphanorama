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
                        IProjectStoreAccess& projects);

  Result<SessionId> Begin(ProjectId project, const CapturePlanSpec& spec) override;
  Result<CapturePlan> GetPlan() const override;
  Result<CaptureGuidance> OnMotion(std::span<const ImuSample> samples) override;
  Result<std::vector<Candidate>> CaptureCell(NodeId node, const BurstSpec& burst) override;
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

  ICoveragePlannerEngine& planner_;
  IPoseEngine& pose_;
  IFrameQualityEngine& quality_;
  ICameraAccess& camera_;
  IMotionSensorAccess& sensor_;
  IFrameStoreAccess& frames_;
  IProjectStoreAccess& projects_;

  bool active_ = false;
  ProjectId project_;
  SessionId session_;
  CapturePlan plan_;
  PoseSample latest_pose_;
  std::map<uint64_t, std::vector<Candidate>> candidates_;

  uint64_t next_session_ = 1;
  uint64_t next_candidate_ = 1;
};

}  // namespace sphanorama
