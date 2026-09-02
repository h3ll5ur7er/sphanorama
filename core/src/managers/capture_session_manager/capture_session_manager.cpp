#include "managers/capture_session_manager/capture_session_manager.h"

#include <algorithm>

namespace sphanorama {
namespace {
constexpr const char* kComponent = "CaptureSessionManager";
}

CaptureSessionManager::CaptureSessionManager(ICoveragePlannerEngine& planner, IPoseEngine& pose,
                                             IFrameQualityEngine& quality, ICameraAccess& camera,
                                             IMotionSensorAccess& sensor,
                                             IFrameStoreAccess& frames,
                                             IProjectStoreAccess& projects)
    : planner_(planner), pose_(pose), quality_(quality), camera_(camera), sensor_(sensor),
      frames_(frames), projects_(projects) {}

Status CaptureSessionManager::RequireSession() const {
  return active_ ? Status::Ok()
                 : Fail(StatusCode::FailedPrecondition, kComponent, "no session in progress");
}

bool CaptureSessionManager::HasNode(NodeId node) const {
  return std::any_of(plan_.nodes.begin(), plan_.nodes.end(),
                     [&](const CoverageNode& n) { return n.id.value == node.value; });
}

std::vector<Candidate> CaptureSessionManager::AllCandidates() const {
  std::vector<Candidate> all;
  for (const auto& [node, candidates] : candidates_) {
    all.insert(all.end(), candidates.begin(), candidates.end());
  }
  return all;
}

void CaptureSessionManager::Discard(std::vector<Candidate>& candidates) {
  // Releasing is not optional housekeeping: a full sphere of bursts is around 15 GB, so a
  // replace-retake that leaked would exhaust the device inside one session.
  for (const auto& candidate : candidates) {
    (void)frames_.Forget(candidate.frame);
  }
  candidates.clear();
}

Result<SessionId> CaptureSessionManager::Begin(ProjectId project, const CapturePlanSpec& spec) {
  // Checked first, and before the camera: End writes this session's document through the project
  // store, and the store creates storage on demand — so beginning against an id nobody created
  // leaves a titleless project behind in the user's list. Asking for camera permission for a
  // session that cannot start would also be the wrong order to fail in.
  if (!projects_.ReadDocument(project, "title").ok()) {
    return Err<SessionId>(StatusCode::NotFound, kComponent, "no such project");
  }

  // The lens decides the tessellation, so the camera is opened before the plan is made rather
  // than when the first burst fires.
  auto opened = camera_.Open(CameraOpenSpec{});
  if (!opened.ok()) return opened.status;

  Intrinsics lens;
  lens.width = opened.value.maxWidth;
  lens.height = opened.value.maxHeight;

  CapturePlanSpec resolved = spec;
  if (resolved.horizontalFovDeg <= 0.0) resolved.horizontalFovDeg = opened.value.horizontalFovDeg;
  if (resolved.verticalFovDeg <= 0.0) resolved.verticalFovDeg = opened.value.verticalFovDeg;

  auto capability = sensor_.Capabilities();
  resolved.motion = capability.ok() ? capability.value : MotionCapability::None;

  SPH_TRY(auto plan, planner_.Plan(resolved, lens));

  // Sensor absence is a supported configuration, not a failure: PoseEngine switches to
  // vision-only and no other component learns the difference (docs/03 UC-4).
  const PoseMode mode =
      resolved.motion == MotionCapability::None ? PoseMode::VisionOnly : PoseMode::Fused;
  SPH_TRY(auto initialPose, pose_.Initial(mode, resolved.motion));
  (void)sensor_.Start(60);
  (void)camera_.StartPreview();

  plan_ = std::move(plan);
  project_ = project;
  session_ = SessionId{next_session_++};
  candidates_.clear();
  pose_state_ = initialPose;
  active_ = true;
  return Ok(session_);
}

Result<CapturePlan> CaptureSessionManager::GetPlan() const {
  if (auto status = RequireSession(); !status.ok()) return status;
  return Ok(plan_);
}

Result<CaptureGuidance> CaptureSessionManager::OnMotion(std::span<const ImuSample> samples) {
  if (auto status = RequireSession(); !status.ok()) return status;

  // An empty batch is the common case, not an error: the capture loop calls this every frame
  // whether or not the sensor produced anything since the last one.
  if (!samples.empty()) {
    SPH_TRY(auto advanced, pose_.Integrate(pose_state_, samples));
    pose_state_ = advanced;
  }

  SPH_TRY(auto guidance, planner_.Locate(pose_state_.pose.orientation, plan_));

  // Stability is advisory: an engine that cannot estimate it yet must not fail the whole call.
  if (auto stability = pose_.Stability(samples); stability.ok()) {
    guidance.stability = stability.value;
  }
  return Ok(guidance);
}

Result<std::vector<Candidate>> CaptureSessionManager::CaptureCell(NodeId node,
                                                                  const BurstSpec& burst) {
  if (auto status = RequireSession(); !status.ok()) return status;
  if (!HasNode(node)) {
    return Err<std::vector<Candidate>>(StatusCode::NotFound, kComponent, "no such cell in the plan");
  }

  // Every frame in a burst must share an exposure, or selection compares brightness rather than
  // sharpness and the blend bands across the cell.
  if (auto locked = camera_.SetLocks(burst.lockExposure, burst.lockWhiteBalance, burst.lockFocus);
      !locked.ok()) {
    return locked;
  }

  SPH_TRY(auto frames, camera_.CaptureBurst(burst));

  std::vector<Candidate>& cell = candidates_[node.value];
  std::vector<Candidate> captured;
  captured.reserve(frames.size());

  for (const auto& frame : frames) {
    Candidate candidate;
    candidate.id = CandidateId{next_candidate_++};
    candidate.node = node;
    candidate.frame = frame;
    candidate.pose = pose_state_.pose;

    NodeContext context;
    context.siblings = cell;
    if (auto scored = quality_.Score(frame, pose_state_.pose, context); scored.ok()) {
      candidate.quality = scored.value;
    }
    cell.push_back(candidate);
    captured.push_back(candidate);
  }

  // Ranking is asked for even though nothing consumes it yet: it is what decides which candidate
  // feeds the build, and leaving the call out would hide a broken selection until Phase 2.
  (void)quality_.Rank(cell, SelectionPolicy{});
  return Ok(std::move(captured));
}

Result<FrameVerdict> CaptureSessionManager::OfferFrame(NodeId node, const FrameRef& frame,
                                                       const PoseSample& pose) {
  if (auto status = RequireSession(); !status.ok()) return status;
  if (!HasNode(node)) {
    return Err<FrameVerdict>(StatusCode::NotFound, kComponent, "no such cell in the plan");
  }

  std::vector<Candidate>& cell = candidates_[node.value];
  Candidate candidate;
  candidate.id = CandidateId{next_candidate_++};
  candidate.node = node;
  candidate.frame = frame;
  candidate.pose = pose;

  NodeContext context;
  context.siblings = cell;
  if (auto scored = quality_.Score(frame, pose, context); scored.ok()) {
    candidate.quality = scored.value;
  }
  cell.push_back(candidate);
  return Ok(FrameVerdict::Accepted);
}

Result<CoverageState> CaptureSessionManager::Coverage() const {
  if (auto status = RequireSession(); !status.ok()) return status;
  const std::vector<Candidate> all = AllCandidates();
  return planner_.Evaluate(plan_, all);
}

Result<std::vector<Candidate>> CaptureSessionManager::Candidates(NodeId node) const {
  if (auto status = RequireSession(); !status.ok()) return status;
  // NotFound rather than an empty success, matching CaptureCell and RequestRetake: a cell that
  // is not in the plan and a cell nobody has captured yet are different answers.
  if (!HasNode(node)) {
    return Err<std::vector<Candidate>>(StatusCode::NotFound, kComponent,
                                       "no such cell in the plan");
  }
  const auto it = candidates_.find(node.value);
  return Ok(it == candidates_.end() ? std::vector<Candidate>{} : it->second);
}

Status CaptureSessionManager::RequestRetake(NodeId node, bool replace) {
  if (auto status = RequireSession(); !status.ok()) return status;
  if (!HasNode(node)) return Fail(StatusCode::NotFound, kComponent, "no such cell in the plan");

  // Keeping evidence is the default: a blurred cell is worth adding to, and only a cell whose
  // frames are actively wrong — a mover walked through it — is worth discarding.
  if (replace) {
    if (auto it = candidates_.find(node.value); it != candidates_.end()) Discard(it->second);
  }
  return Status::Ok();
}

Status CaptureSessionManager::End() {
  if (auto status = RequireSession(); !status.ok()) return status;

  // Metadata is persisted; pixels stay in the frame store under their own handles, which is what
  // makes resuming a metadata read rather than a restore (docs/04 §4.3).
  const std::vector<Candidate> all = AllCandidates();
  (void)projects_.WriteDocument(project_, "session",
                                std::to_string(session_.value) + ":" + std::to_string(all.size()));

  (void)sensor_.Stop();
  (void)camera_.StopPreview();
  (void)camera_.Close();

  active_ = false;
  candidates_.clear();
  plan_ = CapturePlan{};
  return Status::Ok();
}

}  // namespace sphanorama
