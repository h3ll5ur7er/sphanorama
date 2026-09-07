// Shared value types, and the IDL they are generated from (ADR 0009).
//
// tools/contract_gen.py parses this header into the TypeScript mirror and both halves of the
// wire codec — a generated binary format, not FlatBuffers and not JSON (ADR 0013). Editing any
// of those outputs by hand is a drift check away from a red build.
// Pure data: no behaviour, no ownership of pixels, safe to cross the WASM boundary.
#pragma once

#include <cstdint>
#include <utility>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sphanorama {

// ---------------------------------------------------------------- identifiers
// Strong typedefs; a NodeId must never be passable where a FrameId is expected.
template <typename Tag>
struct Id {
  uint64_t value = 0;
  friend bool operator==(Id a, Id b) { return a.value == b.value; }
  friend bool operator<(Id a, Id b) { return a.value < b.value; }
  bool valid() const { return value != 0; }
};
struct SessionTag {};
struct ProjectTag {};
struct NodeTag {};
struct FrameTag {};
struct CandidateTag {};
struct BuildTag {};
struct BufferTag {};

using SessionId   = Id<SessionTag>;
using ProjectId   = Id<ProjectTag>;
using NodeId      = Id<NodeTag>;
using FrameId     = Id<FrameTag>;
using CandidateId = Id<CandidateTag>;
using BuildId     = Id<BuildTag>;
using BufferId    = Id<BufferTag>;

// ---------------------------------------------------------------- error model
// No exceptions cross a layer boundary; every fallible call returns Result<T>.
enum class StatusCode : uint16_t {
  Ok = 0,
  InvalidArgument,
  NotFound,
  FailedPrecondition,
  Cancelled,
  Unsupported,
  SensorPermissionDenied,
  SensorUnavailable,
  CameraUnavailable,
  FrameStoreExhausted,
  StorageQuotaExceeded,
  ComputeUnavailable,
  RegistrationFailed,
  InsufficientCoverage,
  CodecFailure,
  Internal,
};

struct Status {
  StatusCode code = StatusCode::Ok;
  std::string component;        // which service reported it
  std::string detail;           // human-readable, never parsed
  bool ok() const { return code == StatusCode::Ok; }
  static Status Ok() { return {}; }
};

template <typename T>
struct Result {
  Status status;
  T value{};

  Result() = default;
  // Implicit on purpose: `return someFailedStatus;` from a Result-returning function is the
  // single most common line in error handling here, and spelling it out adds nothing.
  Result(Status s) : status(std::move(s)) {}
  Result(Status s, T v) : status(std::move(s)), value(std::move(v)) {}

  bool ok() const { return status.ok(); }
};


// Constructing results. `Ok(v)` and `Err<T>(...)` keep call sites readable; SPH_TRY unwraps a
// Result or propagates its Status, which is what makes a chain of fallible calls tolerable
// without exceptions.
template <typename T>
inline Result<T> Ok(T value) { return Result<T>{Status::Ok(), std::move(value)}; }

template <typename T>
inline Result<T> Err(StatusCode code, const char* component, std::string detail = {}) {
  return Result<T>{Status{code, component, std::move(detail)}};
}

inline Status Fail(StatusCode code, const char* component, std::string detail = {}) {
  return Status{code, component, std::move(detail)};
}

#define SPH_CONCAT_INNER(a, b) a##b
#define SPH_CONCAT(a, b) SPH_CONCAT_INNER(a, b)

// Usage: SPH_TRY(auto plan, planner.Plan(spec, lens));
// Works in any function returning Status or Result<U>.
#define SPH_TRY(decl, expr)                                             \
  auto SPH_CONCAT(sph_try_, __LINE__) = (expr);                         \
  if (!SPH_CONCAT(sph_try_, __LINE__).ok())                             \
    return SPH_CONCAT(sph_try_, __LINE__).status;                       \
  decl = std::move(SPH_CONCAT(sph_try_, __LINE__).value)

// ---------------------------------------------------------------- geometry
struct Vec3 { double x = 0, y = 0, z = 0; };
struct Quat { double w = 1, x = 0, y = 0, z = 0; };   // unit, device -> world

// Estimated during a build, never configured by hand: phone lenses are unknown.
struct Intrinsics {
  double fx = 0, fy = 0, cx = 0, cy = 0;
  double k1 = 0, k2 = 0, k3 = 0, p1 = 0, p2 = 0;   // Brown-Conrady
  int32_t width = 0, height = 0;
  double rollingShutterLineTimeNs = 0;             // 0 == global shutter / unknown
  bool estimated = false;
};

// ---------------------------------------------------------------- sensing
struct ImuSample {
  int64_t timestampNs = 0;
  Vec3 angularVelocity;    // rad/s
  Vec3 acceleration;       // m/s^2
  bool hasMagnetometer = false;
  Vec3 magneticField;

  // Platforms that report a fused absolute orientation rather than raw rates fill these.
  // MotionCapability::OrientationOnly promises exactly that, and without somewhere to put it the
  // contract described a device it had no way to carry data from — which is what the browser
  // turns out to be: DeviceOrientation gives an orientation, not a rate.
  bool hasOrientation = false;
  Quat orientation;

  // Whether `angularVelocity` is a measurement. It is the same distinction `hasOrientation` draws
  // and for the same reason: zero is a real rate, so a zeroed field cannot say whether the device
  // is still or the platform never looked. Reading "not measured" as "not moving" is how a phone
  // mid-swing was once reported as perfectly still (ADR 0025).
  //
  // A platform that reports a fused attitude and no gyroscope leaves this false, and so does one
  // whose DeviceMotionEvent fires with a null rotationRate — which some desktops do.
  bool hasAngularVelocity = false;
};

struct PoseSample {
  int64_t timestampNs = 0;
  Quat orientation;
  Vec3 angularVelocity;
  // How much the orientation above is worth, in [0,1].
  //
  // **Zero means no reading has ever anchored it**, which is not the same as "nothing has moved
  // it": integrating a gyroscope's rates turns the orientation degrees away from where it started
  // and says nothing whatever about where that was. A stream of rates alone reports zero for its
  // whole life, however far it has turned — measured at 8.709° off the identity, still zero — and
  // that is the honest answer, because the direction it is 8.709° away from is one nobody chose.
  //
  // Callers act on the zero rather than on the number's size: `Locate` prefers the cell the camera
  // is inside only for an anchored pose, and names no cell as held otherwise (ADR 0041).
  // `ArmBurst` used to stand its cone check down on a zero and no longer does — there is nothing
  // to check against an identity nobody chose, so there is nothing to allow (ADR 0044). Above
  // zero, 1.0 is an absolute reading and 0.5 is dead reckoning
  // *from* one — drifting away from a direction somebody measured, which is worth aiming with and
  // an unanchored integration is not.
  //
  // The old gloss said zero meant nothing had moved the orientation. An engine written against it
  // reports 0.5 for a heading nobody measured, and every rule above then fires on it: zero of
  // thirty-two cells armable on the shipped tessellation, with the client in aimed mode so nothing
  // on screen says why. This sentence is what a second `IPoseEngine` is written against, so it is
  // the sentence that has to be right.
  double confidence = 0.0;
  bool visuallyCorrected = false;
};

enum class MotionCapability : uint8_t { None, OrientationOnly, GyroAccel, GyroAccelMag };

// ---------------------------------------------------------------- pixels
enum class PixelFormat : uint8_t { Unknown, RGBA8, BGRA8, NV12, I420, Gray8, EncodedJpeg };
enum class Residency  : uint8_t { HeapPinned, HeapEncoded, GpuTexture, Spilled };

// A handle, not a buffer. Pixel bytes never cross the boundary as a value.
//
// Deliberately carries no residency field: residency is store state, not frame identity. A copy
// of this handle taken before a spill would otherwise claim the bytes are still in the heap.
// Ask IFrameStoreAccess::ResidencyOf instead.
struct FrameRef {
  FrameId id;
  BufferId buffer;
  PixelFormat format = PixelFormat::Unknown;
  int32_t width = 0, height = 0, stride = 0;
  int64_t timestampNs = 0;
  uint64_t contentHash = 0;   // build-graph fingerprinting
};

// A frame reduced to something a screen can take — and the one place pixel bytes are a value in
// a contract rather than a handle.
//
// The rule they are an exception to is a rule about cost. A `FrameRef` exists so that a
// 1280x960 RGBA frame — 4.9 MB, and eight of them in a cell — is never serialised by value; a
// review strip handed the frames themselves would move 39 MB across the boundary and hold it in
// the page, against a browser heap ceiling of 128 MB (ADR 0023). The reduction is what pays that
// bill: at a long edge of 128 this is 48 KB, three orders of magnitude down, and the reason for
// the handle no longer applies. What a handle *cannot* do is the thing needed here — the page has
// no frame store to resolve one against, and `IFrameStoreAccess::Pin` reaches no further than the
// core (ADR 0038).
//
// Always `RGBA8` and tightly packed, so the stride is `width * 4` and there is nothing to decode:
// a browser can hand these straight to `ImageData` and a canvas.
struct FramePreview {
  FrameId frame;
  int32_t width = 0, height = 0;
  PixelFormat format = PixelFormat::RGBA8;
  std::vector<uint8_t> pixels;
};

// ---------------------------------------------------------------- capture plan
enum class TessellationStrategy : uint8_t { Rings, Geodesic, Adaptive };

struct CapturePlanSpec {
  TessellationStrategy strategy = TessellationStrategy::Rings;
  double horizontalFovDeg = 0;      // 0 => probe the camera
  double verticalFovDeg = 0;
  double overlapTarget = 0.30;      // fraction
  double acceptanceConeDeg = 4.0;
  bool coverPoles = true;
  MotionCapability motion = MotionCapability::GyroAccel;
};

struct CoverageNode {
  NodeId id;
  Quat targetOrientation;
  double acceptanceConeDeg = 0;
  int32_t ringIndex = 0;
};

struct CapturePlan {
  std::vector<CoverageNode> nodes;
  CapturePlanSpec spec;
};

enum class NodeState : uint8_t { Pending, Capturing, Captured, Satisfied, Flagged, Retaking };

// ---------------------------------------------------------------- candidates
struct QualityScore {
  double sharpness = 0;        // higher is better
  double motionBlur = 0;       // estimated px of smear, lower is better
  double exposureAgreement = 0;
  double alignmentResidual = 0;   // px, vs the cell's other candidates
  double moverPenalty = 0;        // from intra-cell disagreement
  double aggregate = 0;           // the single number selection sorts on
};

struct Candidate {
  CandidateId id;
  NodeId node;
  FrameRef frame;
  PoseSample pose;
  QualityScore quality;
};

struct BurstSpec {
  int32_t frameCount = 5;
  int32_t intervalMs = 80;
  // How long the camera is given to converge after the locks go on, before the first frame is
  // taken. It is a different quantity from the interval — that is how far apart two frames have
  // to be, this is how long one camera takes to settle — and a caller that knows its device
  // should be able to say so.
  //
  // The default is a guess, and it is worth saying so plainly: nobody has measured how long a
  // phone camera takes to converge after a lock. What is known is one device in one scene. A
  // Pixel that holds a focus lock scored its five-frame burst 5.9, 1145, 720, 583, 586 in capture
  // order — the first frame taken 16 ms after arming and roughly 100x less sharp than any of its
  // siblings, the second taken 96 ms after arming and normal. 150 ms is that datapoint with
  // margin. Measuring it per device class is what would replace it.
  int32_t settleMs = 150;
  bool lockExposure = true;
  bool lockWhiteBalance = true;
  bool lockFocus = true;
};

// ---------------------------------------------------------------- guidance
// What the user should do about the cell guidance is naming.
//
// `CellDone` is an *edge*: the manager emits it on the one tick a burst fills, and callers act on
// it once — **unless that tick fails**. Releasing the camera's locks is the last thing a filled
// burst does, and a track that refuses returns that failure from `OnMotion`, so the cell is
// committed and no action announces it. The failure winning is deliberate (a camera left locked is
// the worse problem), which makes this a caller's problem: anything mirroring coverage off
// `CellDone` has to re-read it on a failed tick too. `AlreadyCaptured` is a *level*: the camera is resting inside the cone of a cell that
// already holds a capture, and it is true on every tick the phone stays there. They were briefly
// the same value, which turned a once-per-cell refresh into one per animation frame.
//
// Appended rather than inserted: the wire carries the index.
enum class GuidanceAction : uint8_t {
  Seek, HoldStill, Firing, CellDone, SphereDone, TooFast, AlreadyCaptured, Fire
};
// `Fire` is an *edge*, like `CellDone` and unlike `AlreadyCaptured`: it is reported on the tick a
// dwell completes and not on the ticks either side, and a client arms a burst on it exactly as it
// would have on a press. The manager cannot arm for itself — a burst is paced by the client's
// ticks over a preview frame the client keeps resident (ADR 0018) — so the decision is here and
// the call is the client's.
//
// An edge, but not a once-per-cell one, and the difference is a client's to handle. A `Fire` the
// client could not act on — a lock write that timed out, a camera busy for an instant — is
// offered again after another full dwell, because since ADR 0044 there is no shutter to fall back
// on and a refused arm otherwise strands the capture with the ring full and nothing that will
// ever fire again. A client that arms on every `Fire` is doing the right thing: a burst that
// starts changes the action, so no second `Fire` follows one that took.
//
// Appended, because the wire carries this enum as an index (ADR 0043).

struct CaptureGuidance {
  NodeId targetNode;
  double angularErrorDeg = 0;
  double rollErrorDeg = 0;
  double stability = 0;          // [0,1]
  GuidanceAction action = GuidanceAction::Seek;
  // Whether the orientation this answer was computed from was a measurement at all.
  //
  // It is here because a client has to know which of two answers it is reading and has no other
  // way to find out. With no aim, `angularErrorDeg` is measured from an identity nobody chose and
  // comes back near zero for whichever cell happens to sit there — so a page that drew its
  // reticle from it would show a closed ring on a pose nothing had measured, and a horizon rolled
  // against nothing. It parks both instead.
  //
  // Guessing from "the sensor started" is not the same fact: it is wrong for every tick before
  // the first sample arrives, which is what turned twelve browser tests red when the page tried.
  //
  // It used to gate a shutter as well, on a device that captured without an aim at all. That
  // device is refused now (ADR 0044) and the shutter is gone with it; this field is presentation.
  //
  // Appended rather than inserted, because field order is wire order.
  bool aimKnown = false;
  // How much of the dwell the camera has served on this cell, in [0,1]. One at the moment `action`
  // is `Fire`, zero whenever there is nothing to hold on.
  //
  // Published so the ring the user watches and the trigger that fires are **the same number in the
  // same message**. A client counting its own dwell would be a second copy of a fact this manager
  // already holds, and the two would disagree exactly when it mattered — a progress bar that
  // filled and did not fire, or fired before it filled.
  //
  // It can fill more than once for one cell, and that is not the disagreement above. `Fire` is
  // re-offered after another full dwell when nothing acted on it, so a client whose arm was
  // refused — or is simply still crossing the worker — sees the ring restart from zero and climb
  // again. The ring and the trigger still agree; what a client owes its user in that window is a
  // word about the arm, because a ring that fills twice with nothing happening reads as a control
  // that has stopped working.
  //
  // The dwell is counted here rather than in a client for a reason a client cannot work around:
  // `performance.now()` keeps moving when the sensor stops delivering, so a page counting elapsed
  // time matures its dwell on guidance about a cell the phone may have left. This manager has the
  // sample timestamps and `IClock` side by side, and declines to accumulate over an interval no
  // pose arrived in (ADR 0043).
  //
  // Appended, for the same reason as the field above.
  double heldFraction = 0;
};

struct CoverageState {
  int32_t nodesTotal = 0;
  int32_t nodesSatisfied = 0;
  double coveredSolidAngleFraction = 0;
  std::vector<NodeId> holes;
  std::vector<NodeId> underOverlapped;
};

// ---------------------------------------------------------------- build
enum class Projection : uint8_t { Equirectangular, Cubemap };
enum class QualityTier : uint8_t { Preview, Standard, Maximum };

struct BuildSpec {
  QualityTier tier = QualityTier::Standard;
  Projection projection = Projection::Equirectangular;
  int32_t outputWidth = 8192;
  bool ghostAware = true;
};

enum class BuildStage : uint8_t {
  Queued, Features, PairwiseMatching, GlobalSolve, ExposureCompensation,
  GhostDetection, SeamFinding, Blending, Projecting, Complete, Failed
};

struct BuildProgress {
  BuildId id;
  BuildStage stage = BuildStage::Queued;
  double fraction = 0;
  int32_t tilesReady = 0, tilesTotal = 0;
  Status failure;
};

struct GhostRegion {
  NodeId node;
  double centerAzimuthDeg = 0, centerElevationDeg = 0, radiusDeg = 0;
  double confidence = 0;
};

struct GhostReport { std::vector<GhostRegion> regions; };

// ---------------------------------------------------------------- export
enum class EncodeFormat : uint8_t { Jpeg, Avif, Png };

struct EncodeSpec {
  EncodeFormat format = EncodeFormat::Jpeg;
  int32_t quality = 92;
  bool attachGPanoXmp = false;
  int32_t fullPanoWidth = 0, fullPanoHeight = 0;   // for the GPano metadata block
};

struct PanoramaRef {
  BuildId build;
  Projection projection = Projection::Equirectangular;
  int32_t width = 0, height = 0, tileSize = 0;
  std::vector<FrameRef> tiles;
  FrameRef preview;
};

// --------------------------------------------------- engine value types
// Data has no layer: these cross engine contracts freely, which is exactly why they
// live here rather than in any one engine's header.
enum class PoseMode : uint8_t { Fused, GyroOnly, VisionOnly };

// Everything a pose estimate carries from one batch of samples to the next.
//
// It exists so that IPoseEngine can be a pure function of it. An engine that kept this in members
// would be a stateful engine, which rule 4 in docs/03 §3.3 forbids for a reason worth more than
// the convenience: an estimate you cannot construct is an estimate you cannot replay against a
// recorded log, and replay is how a fusion filter is judged (ADR 0016).
//
// CaptureSessionManager owns one per session, which is where session state belongs.
struct PoseState {
  PoseMode mode = PoseMode::Fused;
  MotionCapability capability = MotionCapability::None;
  PoseSample pose;
  // Whether any sample has arrived at all. It is what the elapsed time between samples is measured
  // from, so the first sample of a stream sets it whether or not it moved anything — a rate has no
  // orientation in it until there is an interval to integrate it over.
  //
  // It is *not* the answer to "is this orientation a measurement": that is `anchored` below, and
  // `PoseSample.confidence` is what callers should read. The two were one flag, and the first
  // rate-only sample of a stream then reported an integrated pose before anything was integrated.
  bool observed = false;
  // Whether this orientation descends from an absolute reading — not whether something moved it.
  //
  // Set the first time an attitude is folded in and never cleared, so it survives the stretches
  // where `absolute` goes false: dead reckoning after a reading is still an estimate *of a
  // direction somebody measured*, which is what confidence 0.5 means.
  //
  // The distinction is the whole of ADR 0042 and it is not the one this field was first written
  // with. It used to mean "something moved the orientation", which dead reckoning also does — so a
  // gyroscope-only stream integrating away from the identity it was born with reported confidence
  // 0.5 for a heading nobody had ever measured, and `ArmBurst` then enforced the acceptance cone
  // against it: zero of thirty-two cells armable on the shipped tessellation, which is ADR 0042's
  // own failure reached through the other door. A rate says how fast the device is turning and
  // nothing about where it started.
  //
  // Callers act on this through `PoseSample.confidence`, which is derived from it: `Locate`
  // prefers aim only for an anchored pose (ADR 0041). What that buys is no longer a sensorless
  // capture — that is refused (ADR 0044) — but a session whose first ticks, and whose rate-only
  // streams, cannot be mistaken for an aim and fire a burst at a cell nobody pointed at.
  bool anchored = false;
  // Whether the pose came from an absolute reading rather than from integrating rates. Confidence
  // is derived from this, so it has to survive between calls.
  bool absolute = false;
  // The gyroscope's zero offset, in rad/s, as far as the engine has been able to observe it. A
  // gyroscope at rest does not read zero, and integrating that offset is how dead reckoning walks
  // away from the truth during the seconds when no absolute reading is arriving to correct it.
  //
  // It is here rather than inside the engine because it is session state, and an engine is
  // stateless per session (docs/03 §3.3 rule 4, ADR 0016). A bias kept in the engine would be
  // shared by every session in the process and would outlive the device being put down.
  Vec3 gyroBias;
};

struct SelectionPolicy {
  double weightSharpness = 1.0;
  double weightMotionBlur = 1.0;
  double weightExposure = 0.5;
  double weightAlignment = 0.75;
  double weightMover = 1.5;
  bool preferPoseAccuracy = true;
};

struct NodeContext {
  Quat targetOrientation;
  std::span<const Candidate> siblings;      // the rest of this cell's burst
  std::span<const Candidate> neighbours;    // selected candidates of adjacent cells
};

struct FeatureSet {
  FrameId frame;
  int32_t count = 0;
  BufferId descriptors;      // opaque, lives in the frame store
  BufferId keypoints;
};

struct PairwiseResult {
  FrameId a, b;
  Quat relativeRotation;
  int32_t inliers = 0;
  double medianResidualPx = 0;
  bool accepted = false;
};

struct GlobalSolution {
  std::vector<FrameId> frames;
  std::vector<Quat> rotations;      // parallel to frames
  Intrinsics intrinsics;            // shared across frames, refined here
  double medianResidualPx = 0;
  int32_t droppedFrames = 0;
};

struct GainMap { std::vector<double> perFrameGain; std::vector<FrameId> frames; };
struct SeamMap { BufferId labelBuffer; int32_t width = 0, height = 0; };

// ------------------------------------------------- platform value types
struct CameraCapabilities {
  // **Zero means "the platform will not say", on every measured field below.** Not "zero" and not
  // a default to improve on: an implementation that cannot answer must answer 0, and one that
  // guesses a plausible number is worse than one that says nothing, because a caller can act on a
  // silence and cannot detect a guess.
  //
  // Written here because five places in this repository cite "the contract's word for 'the
  // platform will not say'" and, until a reviewer went looking, this file said it on one field
  // pair — the one nothing tests. `CaptureSessionManager::ArmBurst` keeps what it had when a
  // refreshed struct answers 0 (ADR 0045), so a second implementation reading only this header
  // had no reason to know that a guessed 30 fps silently removes ADR 0018's burst floor rather
  // than improving on it.
  //
  // The booleans are outside the rule, and that is a gap rather than a decision: `false` here
  // means "no" and "did not say" alike, and there is nowhere to record the difference. See the
  // white-balance and field-of-view entries in `docs/06-roadmap.md`; both want the same change.

  // The mode the camera actually settled on, not the largest it could reach. The coverage plan is
  // sized from these, so they have to describe the frames that will arrive: a sensor maximum the
  // preview never runs at would derive an aspect ratio, and so a ring count, for a frame nobody
  // captures. What the caller asks for is CameraOpenSpec's business; this is the answer.
  int32_t maxWidth = 0, maxHeight = 0;   // 0 when the platform will not say
  // Derived from the frame's own shape where a platform reports no angles — which is every
  // browser — so these move with `maxWidth`/`maxHeight` rather than independently of them, and a
  // caller that keeps one across a silent refresh keeps all four (ADR 0045).
  double horizontalFovDeg = 0, verticalFovDeg = 0;   // 0 when the platform will not say
  bool supportsExposureLock = false;
  bool supportsFocusLock = false;
  bool supportsTorch = false;
  // Frames per second the device settled on. `CaptureSessionManager` floors a burst's interval and
  // its settle with this (ADR 0018, ADR 0032): `PeekPreviewFrame` borrows the *latest* preview
  // frame, so a burst asking for frames faster than the camera makes them fills with duplicates of
  // one exposure, and selection then ranks a frame against copies of itself. 0 turns the floor
  // off, which is the right answer for a platform that will not say and the wrong one for a guess.
  double maxBurstFps = 0;   // 0 when the platform will not say
};

struct CameraOpenSpec {
  int32_t preferredWidth = 0, preferredHeight = 0;
  bool preferRearCamera = true;
};

struct FrameStoreBudget {
  int64_t heapCeilingBytes = 0;    // measured at startup, not assumed
  int64_t heapUsedBytes = 0;
  int64_t spilledBytes = 0;
};

struct ComputeCapabilities {
  bool webgpu = false;
  bool simd = false;
  int32_t threads = 0;         // 0 == single-threaded; a supported mode, not a failure
  int64_t gpuMaxBufferBytes = 0;
};

enum class Kernel : uint16_t {
  WarpEquirect, GaussianPyramid, LaplacianPyramid, MultibandBlend,
  Downsample, AbsDiffMask, GainMap
};

struct KernelArgs {
  std::vector<FrameRef> inputs;
  std::vector<FrameRef> outputs;
  std::vector<double> scalars;
};

// The one place that knows whether work runs on the GPU, on threads, or serially.
// Both backends must produce numerically equivalent results; a differential test enforces it.
// -------------------------------------------------- manager value types
enum class FrameVerdict : uint8_t { Accepted, RejectedQuality, RejectedPose, BurstComplete };

struct ProjectSummary {
  ProjectId id;
  std::string title;
  int64_t createdAtMs = 0;
  int32_t nodesTotal = 0, nodesSatisfied = 0;
  bool hasBuild = false;
  // Whether this project has a capture session written down — something to come back to.
  //
  // The fact, not a promise: `ICaptureSessionManager::Resume` is still free to refuse this
  // document, and a client that treated the flag as a guarantee would have nowhere to put that
  // refusal. What it buys is that a page never has to *try* a resume to find out whether to
  // offer one — a successful attempt opens the camera and starts tracking, so probing commits to
  // a resume nobody asked for (ADR 0036).
  bool hasSession = false;
};

struct ExportSpec {
  EncodeSpec encode;
  std::string filename;
  bool share = false;   // share sheet if available, otherwise download
};

}  // namespace sphanorama
