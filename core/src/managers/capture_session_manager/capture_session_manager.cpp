#include "managers/capture_session_manager/capture_session_manager.h"

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>
#include <string>

namespace sphanorama {
namespace {
constexpr const char* kComponent = "CaptureSessionManager";

// ------------------------------------------------------------------ the session document
//
// What a tab leaves behind. It is deliberately a flat text document rather than the generated
// wire codec: that codec belongs to the boundary (ADR 0013) and a manager reaching into bridge/
// would be a client dependency pointing the wrong way. This is small enough to read in a
// debugger, which is worth something for the one artefact whose job is to outlive the process
// that wrote it.
//
// Versioned by its first line. A document from a build that wrote a different shape is refused
// rather than guessed at — half a restored session is a coverage map that lies about which cells
// hold frames.
constexpr const char* kSessionKey = "session";
constexpr int kSessionVersion = 1;

// Every double round-trips: 17 significant digits is what IEEE-754 needs to come back bit for
// bit, and a pose that drifts in the last place on every reload would be a slow corruption of the
// only thing anchoring a cell to a direction.
std::string Digits(double value) {
  std::ostringstream out;
  out << std::setprecision(17) << value;
  return out.str();
}

template <typename Enum>
bool ReadEnum(std::istringstream& in, int limit, Enum& out) {
  int raw = -1;
  if (!(in >> raw) || raw < 0 || raw >= limit) return false;
  out = static_cast<Enum>(raw);
  return true;
}

struct StoredSession {
  uint64_t session = 0;
  uint64_t nextCandidate = 1;
  CapturePlanSpec spec;
  Intrinsics lens;
  // Only the frames this session's own bursts produced, and the reason is where their bytes are.
  // Cooling spills a cell's own candidates and deliberately leaves offered ones alone (ADR 0023),
  // so an offered frame — a file import, a manual shutter — has nothing in the sink under its
  // name. Writing it down would restore a candidate whose first Pin fails, which is a cell
  // claiming evidence it cannot produce. The caller's handle went away with the tab that held it.
  std::vector<Candidate> candidates;
};

std::string EncodeSession(const StoredSession& stored) {
  std::ostringstream out;
  out << "sphanorama-session " << kSessionVersion << '\n';
  out << "session " << stored.session << ' ' << stored.nextCandidate << '\n';
  out << "lens " << stored.lens.width << ' ' << stored.lens.height << '\n';
  out << "spec " << static_cast<int>(stored.spec.strategy)
      << ' ' << Digits(stored.spec.horizontalFovDeg)
      << ' ' << Digits(stored.spec.verticalFovDeg)
      << ' ' << Digits(stored.spec.overlapTarget)
      << ' ' << Digits(stored.spec.acceptanceConeDeg)
      << ' ' << (stored.spec.coverPoles ? 1 : 0)
      << ' ' << static_cast<int>(stored.spec.motion) << '\n';

  for (const Candidate& candidate : stored.candidates) {
    const FrameRef& frame = candidate.frame;
    const PoseSample& pose = candidate.pose;
    const QualityScore& quality = candidate.quality;
    out << "candidate " << candidate.id.value << ' ' << candidate.node.value
        << ' ' << frame.id.value << ' ' << frame.buffer.value
        << ' ' << static_cast<int>(frame.format)
        << ' ' << frame.width << ' ' << frame.height << ' ' << frame.stride
        << ' ' << frame.timestampNs << ' ' << frame.contentHash
        << ' ' << pose.timestampNs
        << ' ' << Digits(pose.orientation.w) << ' ' << Digits(pose.orientation.x)
        << ' ' << Digits(pose.orientation.y) << ' ' << Digits(pose.orientation.z)
        << ' ' << Digits(pose.angularVelocity.x) << ' ' << Digits(pose.angularVelocity.y)
        << ' ' << Digits(pose.angularVelocity.z)
        << ' ' << Digits(pose.confidence) << ' ' << (pose.visuallyCorrected ? 1 : 0)
        << ' ' << Digits(quality.sharpness) << ' ' << Digits(quality.motionBlur)
        << ' ' << Digits(quality.exposureAgreement) << ' ' << Digits(quality.alignmentResidual)
        << ' ' << Digits(quality.moverPenalty) << ' ' << Digits(quality.aggregate) << '\n';
  }
  return out.str();
}

// All or nothing. A line this does not understand fails the whole read, because the alternative
// is a session that comes back missing the cells whose lines were malformed — and a coverage map
// that quietly lost a cell is worse than one that refuses to load, which at least says so.
bool DecodeSession(const std::string& text, StoredSession& out) {
  std::istringstream lines(text);
  std::string line;

  if (!std::getline(lines, line)) return false;
  {
    std::istringstream header(line);
    std::string tag;
    int version = 0;
    if (!(header >> tag >> version) || tag != "sphanorama-session" || version != kSessionVersion) {
      return false;
    }
  }

  // Nothing may be left over on a line once this build has read what it knows about. A trailing
  // field is a document from a shape this one does not have, and the version gate is the only
  // sanctioned way to read one of those — taking the prefix that fits is how a session comes back
  // missing whatever the extra field was there to say.
  const auto exhausted = [](std::istringstream& in) {
    std::string extra;
    return !(in >> extra);
  };

  bool sawSession = false, sawLens = false, sawSpec = false;
  while (std::getline(lines, line)) {
    if (line.empty()) continue;
    std::istringstream in(line);
    std::string tag;
    in >> tag;
    if (tag == "session") {
      if (!(in >> out.session >> out.nextCandidate)) return false;
      // Zero is not an identity: `Id::valid()` is `value != 0` and every counter in this codebase
      // starts at 1. A document carrying one is one this build cannot honour, and restoring it
      // would seat the session under a name nothing can legitimately hold.
      if (out.session == 0 || out.nextCandidate == 0) return false;
      if (!exhausted(in)) return false;
      sawSession = true;
    } else if (tag == "lens") {
      if (!(in >> out.lens.width >> out.lens.height)) return false;
      if (!exhausted(in)) return false;
      sawLens = true;
    } else if (tag == "spec") {
      int coverPoles = 0;
      if (!ReadEnum(in, 3, out.spec.strategy)) return false;
      if (!(in >> out.spec.horizontalFovDeg >> out.spec.verticalFovDeg >> out.spec.overlapTarget
               >> out.spec.acceptanceConeDeg >> coverPoles)) {
        return false;
      }
      if (!ReadEnum(in, 4, out.spec.motion)) return false;
      out.spec.coverPoles = coverPoles != 0;
      if (!exhausted(in)) return false;
      sawSpec = true;
    } else if (tag == "candidate") {
      Candidate candidate;
      int corrected = 0;
      if (!(in >> candidate.id.value >> candidate.node.value
               >> candidate.frame.id.value >> candidate.frame.buffer.value)) {
        return false;
      }
      if (!ReadEnum(in, 7, candidate.frame.format)) return false;
      if (!(in >> candidate.frame.width >> candidate.frame.height >> candidate.frame.stride
               >> candidate.frame.timestampNs >> candidate.frame.contentHash
               >> candidate.pose.timestampNs
               >> candidate.pose.orientation.w >> candidate.pose.orientation.x
               >> candidate.pose.orientation.y >> candidate.pose.orientation.z
               >> candidate.pose.angularVelocity.x >> candidate.pose.angularVelocity.y
               >> candidate.pose.angularVelocity.z
               >> candidate.pose.confidence >> corrected
               >> candidate.quality.sharpness >> candidate.quality.motionBlur
               >> candidate.quality.exposureAgreement >> candidate.quality.alignmentResidual
               >> candidate.quality.moverPenalty >> candidate.quality.aggregate)) {
        return false;
      }
      // Same rule as the session line above, and it matters more here: a candidate or frame under
      // an invalid identity is one the store would be asked to adopt, and the first thing to go
      // wrong with it would go wrong a long way from this document.
      if (!candidate.id.valid() || !candidate.node.valid() || !candidate.frame.id.valid()
          || !candidate.frame.buffer.valid()) {
        return false;
      }
      if (!exhausted(in)) return false;
      candidate.pose.visuallyCorrected = corrected != 0;
      out.candidates.push_back(candidate);
    } else {
      // An unknown tag is a document from a shape this build does not have, which the version
      // line should already have caught. Refusing rather than skipping keeps that the only way a
      // newer document can be read, instead of half-read.
      return false;
    }
  }
  if (!(sawSession && sawLens && sawSpec)) return false;

  // The candidates win where the two disagree, for the same reason the spill index's slots beat
  // its high-water mark (ADR 0030): only the candidates are acted on. A counter that has fallen
  // behind them — a write torn between the candidate lines and the session line — would have the
  // next burst issue ids naming frames the cell is already holding, and a cell with two
  // candidates under one id has two frames as far as everything downstream can tell. Raised
  // rather than refused, because nothing is lost by raising it and a capture is lost by refusing.
  for (const Candidate& candidate : out.candidates) {
    out.nextCandidate = std::max(out.nextCandidate, candidate.id.value + 1);
  }
  return true;
}

// Puts a cell's candidates into the order the quality engine named.
//
// Quadratic in the size of a cell, which is a burst plus whatever was offered — single digits.
// The alternative is a map keyed by candidate id, which costs more to build than this costs to
// walk at that size and would have to be rebuilt on every change.
//
// A misbehaving ranking cannot change what the cell holds, only the order. It may name fewer
// candidates than there are, and it may name one twice; neither is allowed to lose a captured
// frame or invent one. A short list leaves the strays at the end, and a repeated id is placed
// once — a cell with two entries carrying the same id has two frames as far as everything
// downstream can tell, so the strip would show both in force and cooling and forgetting would
// each run twice over one frame.
void Reorder(std::vector<Candidate>& cell, const std::vector<CandidateId>& order) {
  std::vector<Candidate> ranked;
  ranked.reserve(cell.size());
  const auto alreadyPlaced = [&ranked](const CandidateId id) {
    return std::any_of(ranked.begin(), ranked.end(), [id](const Candidate& placed) {
      return placed.id.value == id.value;
    });
  };

  for (const CandidateId id : order) {
    if (alreadyPlaced(id)) continue;
    const auto found = std::find_if(cell.begin(), cell.end(), [id](const Candidate& candidate) {
      return candidate.id.value == id.value;
    });
    if (found != cell.end()) ranked.push_back(*found);
  }
  for (const Candidate& candidate : cell) {
    if (!alreadyPlaced(candidate.id)) ranked.push_back(candidate);
  }
  cell = std::move(ranked);
}

// One frame's worth at sensor rate, with room to spare. Anything older than the last few
// frames is not worth integrating: the pose it would produce is already stale.
constexpr size_t kDrainBatch = 64;

// Two failures and one Status to say them in.
//
// The cause is what the caller has to act on, so its code is the one that survives; the unlock
// failure is folded into the detail rather than replacing it. Neither is discarded, which is the
// point: a tick that failed *and* left the camera pinned to a burst's exposure is a viewfinder
// the user cannot fix by pointing somewhere else, and reporting only one of the two facts leaves
// nothing holding the other.
Status Also(Status cause, const Status& unlock) {
  if (unlock.ok()) return cause;
  if (!cause.detail.empty()) cause.detail += "; ";
  cause.detail += "and the camera is still locked: " + unlock.detail;
  return cause;
}
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
    // Forgotten, so no longer this manager's to cool. Left behind, the ids would accumulate for
    // the length of a session that retook a lot of cells and mean nothing.
    burst_owned_.erase(candidate.id.value);
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

  // Kept, not read once and dropped: it is the floor on how fast a burst can honestly take
  // distinct frames, and AdvanceBurst is the only place that knows one is being paced.
  max_burst_fps_ = opened.value.maxBurstFps;

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

  auto initialPose = StartTracking(resolved.motion);
  if (!initialPose.ok()) return initialPose.status;

  plan_ = std::move(plan.value);
  project_ = project;
  session_ = SessionId{next_session_++};
  candidates_.clear();
  burst_owned_.clear();
  // Kept because it is what the plan was made from, and a resume has to make the same one: node
  // ids are indices into a particular tessellation, so a sphere replanned from another lens files
  // every restored candidate under a different cell.
  resolved_spec_ = resolved;
  lens_ = lens;
  pose_state_ = initialPose.value;
  active_ = true;
  Checkpoint();
  return Ok(session_);
}

Result<PoseState> CaptureSessionManager::StartTracking(MotionCapability motion) {
  // Sensor absence is a supported configuration, not a failure: PoseEngine switches to
  // vision-only and no other component learns the difference (docs/03 UC-4).
  const PoseMode mode = motion == MotionCapability::None ? PoseMode::VisionOnly : PoseMode::Fused;
  auto initial = pose_.Initial(mode, motion);
  if (!initial.ok()) {
    (void)camera_.Close();
    return initial.status;
  }
  (void)sensor_.Start(60);
  (void)camera_.StartPreview();
  return initial;
}

Result<SessionId> CaptureSessionManager::Resume(ProjectId project) {
  if (active_) {
    return Err<SessionId>(StatusCode::FailedPrecondition, kComponent,
                          "a session is already in progress; end it first");
  }

  // Read before the camera is touched, for the same reason Begin checks the title first: asking
  // for a camera on behalf of a session that cannot start is the worst order to fail in.
  auto document = projects_.ReadDocument(project, kSessionKey);
  if (!document.ok()) {
    return Err<SessionId>(StatusCode::NotFound, kComponent,
                          "this project has no session to resume");
  }
  StoredSession stored;
  if (!DecodeSession(document.value, stored)) {
    // Kept, not deleted. The bytes it names may still be in the sink, and a build that can read
    // this shape may yet come along; throwing away the only record of a capture because this
    // build cannot parse it is the one unrecoverable move available here.
    // `Unsupported` rather than a code of its own: it is what every other "this build does not
    // read that" answer in the core uses, and the shell already turns it into the component's own
    // words rather than a sentence about https (`describeFailure`).
    return Err<SessionId>(StatusCode::Unsupported, kComponent,
                          "this project's session document is from a shape this build cannot read");
  }

  auto opened = camera_.Open(CameraOpenSpec{});
  if (!opened.ok()) return opened.status;
  max_burst_fps_ = opened.value.maxBurstFps;

  // From the document, not from the camera. This is the whole reason the spec is stored.
  auto plan = planner_.Plan(stored.spec, stored.lens);
  if (!plan.ok()) {
    (void)camera_.Close();
    return plan.status;
  }

  // Every candidate has to name a cell of this plan. It should be unreachable — the plan is made
  // from the spec the document carries — but a document naming a node the sphere does not have
  // would put candidates somewhere `Coverage` counts and nothing can ever aim at.
  for (const Candidate& candidate : stored.candidates) {
    const bool known = std::any_of(plan.value.nodes.begin(), plan.value.nodes.end(),
                                   [&candidate](const CoverageNode& planned) {
                                     return planned.id.value == candidate.node.value;
                                   });
    if (!known) {
      (void)camera_.Close();
      return Err<SessionId>(StatusCode::Unsupported, kComponent,
                            "this project's session document names a cell this plan does not have");
    }
  }

  // The frames come back before the session does. A store that will not take them leaves a
  // coverage map claiming cells whose pixels nothing can reach, which is worse than refusing:
  // the capture would look finished and build into nothing.
  //
  // A failure here leaves the frames it already took, on purpose, and an earlier draft of this
  // undid them with `Forget` — which takes the sink's copy with it. That is the capture: undoing
  // a half-finished restore that way would delete the user's sphere in order to tidy up after a
  // failure, and every later attempt would pin-fail against a file with nothing in it.
  //
  // They are not orphans either. The document still names them, which is the whole point of it,
  // so a frame taken back under the identity it was given is exactly the frame the next attempt
  // asks for — and `Adopt` is idempotent for one it already holds, so the retry is cheaper rather
  // than impossible.
  for (const Candidate& candidate : stored.candidates) {
    if (auto taken = frames_.Adopt(candidate.frame); !taken.ok()) {
      (void)camera_.Close();
      return Err<SessionId>(taken.code, kComponent,
                            "a captured frame could not be taken back: " + taken.detail);
    }
  }

  // The live capability rather than the stored one: the document says which sphere is being
  // captured, never what the device it came back on can sense.
  auto capability = sensor_.Capabilities();
  auto initialPose = StartTracking(capability.ok() ? capability.value : MotionCapability::None);
  if (!initialPose.ok()) return initialPose.status;

  plan_ = std::move(plan.value);
  project_ = project;
  session_ = SessionId{stored.session};
  resolved_spec_ = stored.spec;
  lens_ = stored.lens;
  candidates_.clear();
  burst_owned_.clear();
  for (const Candidate& candidate : stored.candidates) {
    candidates_[candidate.node.value].push_back(candidate);
    // Everything in the document is this session's own — that is the only thing it holds — so
    // cooling and discarding treat a restored frame exactly as they would the burst that made it.
    burst_owned_.insert(candidate.id.value);
  }
  // Stepped past what the document already used, or the next burst of this session would issue
  // candidate ids that name restored frames.
  next_candidate_ = stored.nextCandidate;
  if (stored.session >= next_session_) next_session_ = stored.session + 1;
  pose_state_ = initialPose.value;
  active_ = true;
  return Ok(session_);
}

void CaptureSessionManager::Checkpoint() const {
  StoredSession stored;
  stored.session = session_.value;
  stored.nextCandidate = next_candidate_;
  stored.spec = resolved_spec_;
  stored.lens = lens_;
  for (const Candidate& candidate : AllCandidates()) {
    if (burst_owned_.count(candidate.id.value) != 0) stored.candidates.push_back(candidate);
  }
  // Not reported, and there is nobody to report it to: this runs on the way out of a burst the
  // caller has already been told about. A write that fails costs the resume, not the capture —
  // the frames are still in the store and the session is still live.
  (void)projects_.WriteDocument(project_, kSessionKey, EncodeSession(stored));
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

  // Coverage is asked for on every tick rather than cached, so guidance cannot aim at a cell that
  // a burst finished a moment ago. It is a scan of the plan against the candidates — a few
  // thousand comparisons for a sphere this size — against a cached answer that would have to be
  // invalidated at every one of the five places candidates change, and a missed one would put the
  // reticle back on a captured cell with nothing to show it had happened.
  auto covered = planner_.Evaluate(plan_, AllCandidates());
  if (!covered.ok()) return Abandon(covered.status);

  auto located = planner_.Locate(pose_state_.pose.orientation, plan_, covered.value);
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
  last_frame_ns_ = clock_.MonotonicNs() - BurstIntervalNs();
  return Status::Ok();
}

int64_t CaptureSessionManager::BurstIntervalNs() const {
  int64_t interval = static_cast<int64_t>(burst_spec_.intervalMs) * 1000000;

  // The camera's own rate is a second floor under the spec's, and it is the one that bites. A
  // burst asking for 0 ms between frames on a 30 fps camera would take a frame on every tick of a
  // 60 Hz loop — and PeekPreviewFrame borrows the *latest* frame, so half of those ticks would
  // hand back the one already taken. The burst would fill with duplicates and selection would
  // rank a single exposure against copies of itself, which is worse than a slower burst because
  // it looks like a fast one.
  //
  // Zero means the platform will not say, in which case the tick rate is the only floor there is.
  if (max_burst_fps_ > 0) {
    const int64_t byCamera = static_cast<int64_t>(1000000000.0 / max_burst_fps_);
    if (byCamera > interval) interval = byCamera;
  }
  return interval;
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

  // Every way out of a burst passes through here, which is why the cooling is here and not on the
  // path that succeeds. A retake scores against its siblings, and scoring reads pixels — so the
  // cell's spilled frames are faulted back into the heap by the attempt itself. A burst that then
  // ends any other way would leave them resident for the rest of the session, holding the ceiling
  // this policy exists to keep clear, by the one route that skipped the policy.
  //
  // Nothing reads a ranked cell's pixels until the build or the review client asks, and both of
  // those go through Pin, which faults them back in.
  Cool(candidates_[burst_node_.value]);

  // Written down here rather than only at End, because End is the one way out of a session a
  // reload never takes. A burst that finished is evidence, and what a crash costs is whatever
  // happened after the last one of these.
  Checkpoint();

  // Attempted unconditionally and reported rather than discarded. A burst that ends leaving the
  // exposure pinned to whatever the cell needed is a viewfinder the user cannot fix by pointing
  // somewhere else, and a caller told the cell was captured would have no reason to look.
  return camera_.SetLocks(false, false, false);
}

Status CaptureSessionManager::Abandon(const Status& cause) {
  // Disarm is a no-op when nothing is armed, so this needs no guard — and its failure is folded
  // in rather than dropped. A pose failure that coincides with an unlock rejection used to return
  // the pose failure alone, and the client stops ticking on a failed call: the lock would then
  // outlive the session with nobody informed it was ever taken.
  return Also(cause, Disarm(true));
}

void CaptureSessionManager::Cool(const std::vector<Candidate>& candidates) {
  for (const auto& candidate : candidates) {
    // Only what a burst here produced. An offered frame is the caller's, and changing the
    // residency of a borrowed handle is a surprise the borrower has no way to expect.
    if (burst_owned_.count(candidate.id.value) == 0) continue;
    // Not reported, and the reason is the same for every way this can fail: the cell is already
    // captured by the time this runs. A store with no sink answers Unsupported, which is the
    // native build and any browser whose OPFS handle did not open — a supported configuration
    // that caps a sphere at what fits in RAM rather than a fault. A sink that refuses the write
    // is a phone out of quota, and it leaves the frame exactly where it was: still in the heap,
    // still readable, still this cell's evidence.
    //
    // What is lost by discarding it is the cause rather than the consequence. The heap keeps
    // bytes it hoped to give back, and the next allocation that does not fit is refused — but
    // FrameStoreExhausted names the ceiling and nothing else, so a sink out of quota looks
    // exactly like a capture too large for the device. Carrying the reason that far means the
    // store remembering a refused spill and saying so when it runs out, which is a diagnostic it
    // does not have yet (ADR 0023).
    (void)frames_.Demote(candidate.frame, Residency::Spilled);
  }
}

Result<bool> CaptureSessionManager::AdvanceBurst() {
  const int64_t now = clock_.MonotonicNs();
  const int64_t due = last_frame_ns_ + BurstIntervalNs();
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
  auto ranked = quality_.Rank(cell, SelectionPolicy{});
  if (!ranked.ok()) {
    // Back to exactly what was there before this line, which may include a frame offered while
    // the burst was in flight. Disarm forgets the pending frames; it must not touch that one.
    cell.resize(before);
    return Abandon(ranked.status);
  }
  // Kept rather than discarded, which it was. Deciding what "best" means is V6's and the answer
  // was being computed and thrown away, so `Candidates` handed back the order the shutter fired
  // in — an order that is not an opinion about anything, and that a client wanting a better one
  // could only improve by doing the engine's job itself.
  Reorder(cell, ranked.value);

  // Ranked, so these frames are the manager's evidence rather than a burst that might roll back,
  // and cooling them is now its business. Disarm is what does it, on this path and every other.
  for (const auto& taken : pending_) burst_owned_.insert(taken.id.value);

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

  // Ranked with the rest, because an imported frame is evidence like any other and may be the
  // best of the set — appending it unranked would hold a better frame behind worse ones for the
  // rest of the session. Rolled back when nobody can rank it, symmetrically with the burst path:
  // a cell holding a candidate the selection engine has already failed on is worse than a
  // rejected offer, because the failure is invisible and the strip is in an order nothing chose.
  auto ranked = quality_.Rank(cell, SelectionPolicy{});
  if (!ranked.ok()) {
    cell.pop_back();
    return ranked.status;
  }
  Reorder(cell, ranked.value);
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
  if (firing_ && burst_node_.value == node.value) {
    // Reported, and before the candidate set is touched. A retake that answered Ok while the
    // camera stayed pinned to the abandoned burst's exposure would send the caller off to re-aim
    // at a viewfinder that cannot respond. The burst itself is gone either way, so a caller that
    // sees this failure and asks again gets the retake it wanted on the second attempt.
    if (auto disarmed = Disarm(true); !disarmed.ok()) return disarmed;
  }

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
  //
  // Held rather than returned here: the rest of this must happen whatever the unlock did, or a
  // session that failed halfway through ending would leave the sensor running and the camera
  // open, with `active_` still true and no way to end it.
  const Status disarmed = Disarm(true);

  // Metadata is persisted; the pixels stay wherever the frame store has put them, which is what
  // makes resuming a document read plus an Adopt rather than a restore (docs/04 §4.3).
  Checkpoint();

  (void)sensor_.Stop();
  (void)camera_.StopPreview();
  const Status closed = camera_.Close();

  active_ = false;
  max_burst_fps_ = 0;
  candidates_.clear();
  burst_owned_.clear();
  plan_ = CapturePlan{};

  // The session is over regardless — every field above is cleared before this returns, so there
  // is nothing here for a caller to retry. What is left worth reporting is the camera, and only
  // while it is still open: a close that succeeded stopped the track and took the burst's locks
  // with it, which makes a failed unlock moot rather than hidden. A close that *failed* leaves a
  // camera that is both open and possibly still locked, and that is worth saying even though the
  // session ended cleanly.
  if (!closed.ok()) return Also(closed, disarmed);
  return Status::Ok();
}

}  // namespace sphanorama
