#include "managers/capture_session_manager/capture_session_manager.h"

#include <algorithm>
#include <array>

namespace sphanorama {
namespace {
constexpr const char* kComponent = "CaptureSessionManager";

// One frame's worth at sensor rate, with room to spare. Anything older than the last few
// frames is not worth integrating: the pose it would produce is already stale.
constexpr size_t kDrainBatch = 64;
}

CaptureSessionManager::CaptureSessionManager(ICoveragePlannerEngine& planner, IPoseEngine& pose,
                                             IFrameQualityEngine& quality, ICameraAccess& camera,
                                             IMotionSensorAccess& sensor,
                                             IFrameStoreAccess& frames,
                                             IProjectStoreAccess& projects, IClock& clock)
    : planner_(planner), pose_(pose), quality_(quality), camera_(camera), sensor_(sensor),
      frames_(frames), projects_(projects), clock_(clock) {}

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
  if (active_) {
    // Replacing a live session silently dropped its candidate map, leaving every frame in it
    // pinned in the store with nothing holding the handles — a full sphere of bursts leaked on a
    // double-tap. Ending a session is how you end one; there is no second way.
    return Err<SessionId>(StatusCode::FailedPrecondition, kComponent,
                          "a session is already in progress; end it first");
  }

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

  // Everything from here can fail with the camera already open, because the lens has to be read
  // before the plan can be made. Returning without closing it leaves the indicator lit for a
  // session that never started — which the user reads, correctly, as the app watching them.
  // Rejecting an unsupported strategy or a nonsense field of view is what makes this reachable.
  auto plan = planner_.Plan(resolved, lens);
  if (!plan.ok()) {
    (void)camera_.Close();
    return plan.status;
  }

  // Sensor absence is a supported configuration, not a failure: PoseEngine switches to
  // vision-only and no other component learns the difference (docs/03 UC-4).
  const PoseMode mode =
      resolved.motion == MotionCapability::None ? PoseMode::VisionOnly : PoseMode::Fused;
  auto initialPose = pose_.Initial(mode, resolved.motion);
  if (!initialPose.ok()) {
    (void)camera_.Close();
    return initialPose.status;
  }

  (void)sensor_.Start(60);
  (void)camera_.StartPreview();

  plan_ = std::move(plan.value);
  project_ = project;
  session_ = SessionId{next_session_++};
  candidates_.clear();
  pose_state_ = initialPose.value;
  active_ = true;
  return Ok(session_);
}

Result<CapturePlan> CaptureSessionManager::GetPlan() const {
  if (auto status = RequireSession(); !status.ok()) return status;
  return Ok(plan_);
}

Result<CaptureGuidance> CaptureSessionManager::OnMotion(std::span<const ImuSample> samples) {
  if (auto status = RequireSession(); !status.ok()) return status;

  // A client that already holds samples passes them; one that does not lets the manager pull.
  // Both exist for real: the browser drains its own event buffer in JavaScript, while the bench
  // replays a recorded log through the port. Accepting only pushed samples would leave
  // IMotionSensorAccess::Drain unimplementable on the browser and unused everywhere else, which
  // is a contract with a limb nothing can reach.
  //
  // Never both in one call — draining underneath a client that pushed would integrate every
  // sample twice and advance the pose at double rate.
  std::array<ImuSample, kDrainBatch> pulled{};
  std::span<const ImuSample> batch = samples;
  if (samples.empty()) {
    if (auto drained = sensor_.Drain(std::span<ImuSample>(pulled)); drained.ok()) {
      batch = std::span<const ImuSample>(pulled.data(), static_cast<size_t>(drained.value));
    }
    // A port that cannot be pulled is not a failure. It means the client is the push kind, and
    // this call is the empty-batch case below.
  }

  // An empty batch is the common case, not an error: the capture loop calls this every frame
  // whether or not the sensor produced anything since the last one.
  //
  // Note what every failure below goes through. A burst is advanced at the end of this call, so a
  // pose or planner failure returns before it — and SPH_TRY here would leave the burst armed with
  // the exposure locked. The client stops ticking once a call fails, so nothing would ever reach
  // the cleanup: the lock would outlive the session.
  if (!batch.empty()) {
    auto advanced = pose_.Integrate(pose_state_, batch);
    if (!advanced.ok()) return Abandon(advanced.status);
    pose_state_ = advanced.value;
  }

  auto located = planner_.Locate(pose_state_.pose.orientation, plan_);
  if (!located.ok()) return Abandon(located.status);
  CaptureGuidance guidance = located.value;

  // Stability is advisory: an engine that cannot estimate it yet must not fail the whole call.
  if (auto stability = pose_.Stability(batch); stability.ok()) {
    guidance.stability = stability.value;
  }

  // The burst rides on this tick because it is the only call the client makes often enough to
  // pace one (ADR 0018). It is folded in after the pose so the frame is scored against the
  // attitude this batch produced, not the previous one.
  if (firing_) {
    // The armed cell, not the nearest one: a burst that retargeted itself mid-flight because the
    // user drifted would end up with candidates from two cells in one set.
    guidance.targetNode = burst_node_;
    // AdvanceBurst abandons the burst itself on the way out, so this needs no guard of its own.
    SPH_TRY(const bool completed, AdvanceBurst());
    guidance.action = completed ? GuidanceAction::CellDone : GuidanceAction::Firing;
  }
  return Ok(guidance);
}

Status CaptureSessionManager::ArmBurst(NodeId node, const BurstSpec& burst) {
  if (auto status = RequireSession(); !status.ok()) return status;
  if (!HasNode(node)) return Fail(StatusCode::NotFound, kComponent, "no such cell in the plan");
  if (burst.frameCount <= 0) {
    return Fail(StatusCode::InvalidArgument, kComponent, "a burst needs at least one frame");
  }
  if (burst.intervalMs < 0) {
    // Checked before the locks are taken, and checked at all because the arithmetic below turns a
    // negative interval into a frame that is always overdue — so instead of failing it would
    // quietly capture on every tick, at whatever rate the client happens to run.
    return Fail(StatusCode::InvalidArgument, kComponent, "a burst interval cannot be negative");
  }
  if (firing_) {
    // Refused rather than replacing: the burst in flight holds the exposure lock and frames it
    // has not committed, and a second arm would strand both. One burst at a time is also all a
    // single camera can honestly serve.
    return Fail(StatusCode::FailedPrecondition, kComponent, "a burst is already in flight");
  }

  // Every frame in a burst must share an exposure, or selection compares brightness rather than
  // sharpness and the blend bands across the cell. The lock is taken here and held across the
  // ticks that follow, which is why Disarm exists.
  if (auto locked = camera_.SetLocks(burst.lockExposure, burst.lockWhiteBalance, burst.lockFocus);
      !locked.ok()) {
    return locked;
  }

  firing_ = true;
  burst_node_ = node;
  burst_spec_ = burst;
  pending_.clear();
  // Far enough in the past that the first frame is taken on the next tick rather than after one
  // interval: the burst starts when it is armed.
  last_frame_ns_ = clock_.MonotonicNs() - static_cast<int64_t>(burst.intervalMs) * 1000000;
  return Status::Ok();
}

Status CaptureSessionManager::Disarm(bool rollBack) {
  if (!firing_) return Status::Ok();
  if (rollBack) {
    // Only this burst's frames, and they were never in the cell — which is what makes this
    // exact rather than an index into a vector somebody else can also append to.
    for (const auto& candidate : pending_) (void)frames_.Forget(candidate.frame);
  }
  pending_.clear();
  firing_ = false;
  // Attempted unconditionally and reported rather than discarded. A burst that ends leaving the
  // exposure pinned to whatever the cell needed is a viewfinder the user cannot fix by pointing
  // somewhere else, and a caller told the cell was captured would have no reason to look.
  return camera_.SetLocks(false, false, false);
}

Status CaptureSessionManager::Abandon(const Status& cause) {
  if (firing_) (void)Disarm(true);
  return cause;
}

Result<bool> CaptureSessionManager::AdvanceBurst() {
  const int64_t now = clock_.MonotonicNs();
  const int64_t due = last_frame_ns_ + static_cast<int64_t>(burst_spec_.intervalMs) * 1000000;
  if (now < due) return Ok(false);   // still inside the interval; the burst keeps waiting

  auto frame = camera_.PeekPreviewFrame();
  if (!frame.ok()) {
    // Not a dropped tick. Preview is running by the time a burst is armed, so a camera that
    // cannot produce a frame has failed — and retrying every tick forever would hold the exposure
    // lock while doing it.
    return Abandon(frame.status);
  }
  last_frame_ns_ = now;

  Candidate candidate;
  candidate.id = CandidateId{next_candidate_++};
  candidate.node = burst_node_;
  candidate.frame = frame.value;
  candidate.pose = pose_state_.pose;

  // Scored against the cell as it stands plus what this burst has already taken: siblings are
  // what an inter-candidate comparison needs, and the frames in flight are siblings whether or
  // not they have been committed yet.
  // One vector so the span has something to point at. It copies the cell per frame, which is a
  // burst's worth of candidates and not a sphere's — and the alternative is scoring a frame
  // against siblings that leave out the rest of its own burst.
  std::vector<Candidate> siblings = candidates_[burst_node_.value];
  siblings.insert(siblings.end(), pending_.begin(), pending_.end());
  NodeContext context;
  context.siblings = siblings;
  auto scored = quality_.Score(frame.value, pose_state_.pose, context);
  if (!scored.ok()) {
    // A default QualityScore is what a genuinely terrible frame gets, so using it for "the scorer
    // failed" makes the two indistinguishable — and the cell would then count toward coverage,
    // reporting a sphere complete that nothing ever judged. Selection would pick a best frame
    // from a set of placeholders. The frames this burst took go back with it.
    (void)frames_.Forget(frame.value);
    return Abandon(scored.status);
  }
  candidate.quality = scored.value;
  pending_.push_back(candidate);

  if (static_cast<int32_t>(pending_.size()) < burst_spec_.frameCount) return Ok(false);

  // The burst is full, so it becomes evidence: appended to the cell and ranked as one set. Until
  // this line the frames are not in the cell at all, which is what keeps Coverage from reporting
  // a cell satisfied by a burst that could still roll back.
  std::vector<Candidate>& cell = candidates_[burst_node_.value];
  const size_t before = cell.size();
  cell.insert(cell.end(), pending_.begin(), pending_.end());

  // Ranking is what turns a burst into a choice, so a set nobody could rank is not a captured
  // cell — and discarding the failure would leave the cell holding candidates the selection
  // engine had already failed on, with the burst reporting success.
  if (auto ranked = quality_.Rank(cell, SelectionPolicy{}); !ranked.ok()) {
    // Back to exactly what was there before this line, which may include a frame offered while
    // the burst was in flight. Disarm forgets the pending frames; it must not touch that one.
    cell.resize(before);
    return Abandon(ranked.status);
  }

  // Committed, so the rollback is off the table: Disarm keeps the frames and only unlocks. If the
  // unlock fails the cell is still captured and the caller is told anyway, because a camera left
  // locked is not something to discover later.
  if (auto unlocked = Disarm(false); !unlocked.ok()) return unlocked;
  return Ok(true);
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
  auto scored = quality_.Score(frame, pose, context);
  if (!scored.ok()) {
    // Same reasoning as CaptureCell, minus the release: this frame came from the caller, who
    // still owns it. Forgetting someone else's handle would be the worse bug.
    return scored.status;
  }
  candidate.quality = scored.value;
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
  // A retake of the cell being fired at aborts that burst first: its frames were taken for the
  // set the caller is about to change, and its rollback mark points into a vector Discard is
  // about to empty.
  if (firing_ && burst_node_.value == node.value) (void)Disarm(true);

  if (replace) {
    if (auto it = candidates_.find(node.value); it != candidates_.end()) Discard(it->second);
  }
  return Status::Ok();
}

Status CaptureSessionManager::End() {
  if (auto status = RequireSession(); !status.ok()) return status;

  // A burst still in flight never became a ranked set, so its frames are not evidence: keeping
  // them would let a half-burst count toward coverage on the next resume. It also puts the
  // camera's locks back before the camera is closed.
  (void)Disarm(true);

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
