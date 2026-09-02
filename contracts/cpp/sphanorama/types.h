// Shared value types. Generated into TypeScript and FlatBuffers by tools/codegen (Phase 0).
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
};

struct PoseSample {
  int64_t timestampNs = 0;
  Quat orientation;
  Vec3 angularVelocity;
  double confidence = 0.0;   // [0,1]
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
  bool lockExposure = true;
  bool lockWhiteBalance = true;
  bool lockFocus = true;
};

// ---------------------------------------------------------------- guidance
enum class GuidanceAction : uint8_t { Seek, HoldStill, Firing, CellDone, SphereDone, TooFast };

struct CaptureGuidance {
  NodeId targetNode;
  double angularErrorDeg = 0;
  double rollErrorDeg = 0;
  double stability = 0;          // [0,1]
  GuidanceAction action = GuidanceAction::Seek;
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
  // Whether any sample has been folded in at all. Distinguishes "identity because nothing has
  // been seen" from "identity because the device is level and facing north".
  bool observed = false;
  // Whether the pose came from an absolute reading rather than from integrating rates. Confidence
  // is derived from this, so it has to survive between calls.
  bool absolute = false;
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
  int32_t maxWidth = 0, maxHeight = 0;
  double horizontalFovDeg = 0, verticalFovDeg = 0;   // 0 when the platform will not say
  bool supportsExposureLock = false;
  bool supportsFocusLock = false;
  bool supportsTorch = false;
  double maxBurstFps = 0;
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
};

struct ExportSpec {
  EncodeSpec encode;
  std::string filename;
  bool share = false;   // share sheet if available, otherwise download
};

}  // namespace sphanorama
