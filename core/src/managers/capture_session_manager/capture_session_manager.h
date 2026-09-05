#pragma once

#include <map>
#include <set>
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
  Result<SessionId> Resume(ProjectId project) override;
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
  // Writes the session down, so a tab that dies has left something to come back to. Called when a
  // cell is committed rather than only at End: a burst that finished is evidence, and a crash
  // costs whatever happened after the last one of these.
  void Checkpoint() const;
  // Everything both entry points do once a plan exists: the pose the session starts from, the
  // sensor, the preview. It closes the camera itself if the pose engine refuses, because by here
  // the camera is already open and a session that cannot start must not leave the indicator lit.
  //
  // The motion capability is the live one on both paths, including a resume: a stored session
  // says which sphere is being captured, never what the device it comes back on can sense.
  Result<PoseState> StartTracking(MotionCapability motion);
  bool HasNode(NodeId node) const;
  std::vector<Candidate> AllCandidates() const;
  void Discard(std::vector<Candidate>& candidates);
  // Sends a cell's own frames to whatever cheaper tier the store has. The session knows when a
  // frame stops being looked at — that is what a sequence is — and the store knows what cheaper
  // means; this is the one line where those two facts meet.
  //
  // "Its own" is the load-bearing word: a cell holds offered candidates too, and those belong to
  // the caller.
  void Cool(const std::vector<Candidate>& candidates);

  // How far apart the armed burst's frames have to be, in nanoseconds: the larger of what the
  // spec asked for and what the camera says it can deliver.
  int64_t BurstIntervalNs() const;
  // How long the armed burst waits after arming before its first frame, in nanoseconds, on the
  // same two floors — a different quantity from the interval, and the camera's rate bounds it for
  // a different reason (ADR 0032).
  int64_t BurstSettleNs() const;
  // Takes at most one frame for the armed burst. Reports whether the burst finished on this tick,
  // so OnMotion can say CellDone exactly once.
  Result<bool> AdvanceBurst();
  // Puts the camera back and forgets a burst in progress. Every path out of a burst goes through
  // here — completion, failure, retake, end of session — because a burst that leaves the exposure
  // locked is a viewfinder the user cannot fix by pointing somewhere else.
  //
  // Returns whether the unlock itself succeeded. The port is fallible, and a caller that
  // discarded this would report a captured cell while the camera stayed locked, with nothing left
  // holding the knowledge that it is.
  Status Disarm(bool rollBack);
  // Abandons an armed burst and hands back the status that caused it. Every early return from a
  // tick goes through here: a burst is only advanced at the end of OnMotion, so a pose or planner
  // failure before that would otherwise leave it armed and locked with the client — which stops
  // ticking once a call fails — never able to reach the cleanup.
  Status Abandon(const Status& cause);

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
  // The plan spec as it was resolved when the session began, kept because it is what the plan was
  // made from. A resumed session replans from this rather than from the camera in front of it.
  CapturePlanSpec resolved_spec_;
  Intrinsics lens_;

  // Which candidates this manager's own bursts produced, by id.
  //
  // A cell is not an ownership boundary: `OfferFrame` appends the caller's candidates to the same
  // vector, and their frames are the caller's handles — demoting one sends a frame somebody else
  // is holding to a sink they do not know exists, so their next Pin faults from it or is refused
  // at a ceiling they cannot see. The manager's note on `pending_` says the same thing about a
  // rollback mark; this is the same vector and the same reason.
  //
  // Ids rather than a second vector of candidates, because what is needed is a membership test
  // and a copy would be a second place for the truth to live.
  std::set<uint64_t> burst_owned_;

  // What the camera said it can deliver when it was opened, in frames per second; 0 when the
  // platform will not say. It is a floor on a burst's waits — the interval between its frames and
  // the settle before its first — and nothing else reads it.
  double max_burst_fps_ = 0;

  // The burst in flight, if any. It is session state and it lives here for the same reason the
  // pose does: a manager is the only thing allowed to be stateful (docs/03 §3.3 rule 4).
  bool firing_ = false;
  NodeId burst_node_;
  BurstSpec burst_spec_;
  // The frames this burst has taken, held apart from the cell until the whole burst ranks.
  //
  // They were in the cell, marked by an index, and that was wrong twice over. Coverage() counts a
  // cell satisfied as soon as any candidate exists for it, so the first frame of a burst reported
  // the cell complete while the burst could still roll back. And an index is not an ownership
  // boundary: OfferFrame appends to the same vector, so a frame the caller still owned could land
  // past the mark and be forgotten by a rollback that had no business touching it.
  std::vector<Candidate> pending_;
  // When the next frame of the armed burst is due. Arming sets it a settle ahead and each frame
  // taken sets it an interval ahead, so the one comparison in AdvanceBurst serves both waits —
  // and there is no moment at which the deadline has to be inferred from a frame that has not
  // been taken yet, which is what the old "last frame" reading needed a backdated clock for.
  int64_t next_frame_ns_ = 0;

  uint64_t next_session_ = 1;
  uint64_t next_candidate_ = 1;
};

}  // namespace sphanorama
