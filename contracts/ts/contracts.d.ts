/**
 * GENERATED FILE — DO NOT EDIT.
 *
 * Produced from the C++ contract headers by tools/contract_gen.py, which is the mechanism that
 * keeps this mirror from drifting (ADR 0009). To change anything here, change the header and
 * regenerate:
 *
 *     python3 tools/contract_gen.py
 *
 * Only interfaces marked `// @boundary` in C++ appear here: engines and the utilities bar never
 * cross into JavaScript.
 *
 * Identifiers are branded numbers rather than bigints; they are minted by the core and stay well
 * below 2^53. Content hashes are bigint, because the bits a double drops are the ones that decide
 * whether a build stage is reused.
 */

export type Result<T> = { ok: true; value: T } | { ok: false; status: Status };

export type SessionId = number & { readonly __brand: 'SessionId' };

export type ProjectId = number & { readonly __brand: 'ProjectId' };

export type NodeId = number & { readonly __brand: 'NodeId' };

export type FrameId = number & { readonly __brand: 'FrameId' };

export type CandidateId = number & { readonly __brand: 'CandidateId' };

export type BuildId = number & { readonly __brand: 'BuildId' };

export type BufferId = number & { readonly __brand: 'BufferId' };

/**
 * ---------------------------------------------------------------- error model
 * No exceptions cross a layer boundary; every fallible call returns Result<T>.
 */
export type StatusCode = 'Ok' | 'InvalidArgument' | 'NotFound' | 'FailedPrecondition' | 'Cancelled' | 'Unsupported' | 'SensorPermissionDenied' | 'SensorUnavailable' | 'CameraUnavailable' | 'FrameStoreExhausted' | 'StorageQuotaExceeded' | 'ComputeUnavailable' | 'RegistrationFailed' | 'InsufficientCoverage' | 'CodecFailure' | 'Internal';

export interface Status {
  code: StatusCode;
  /** which service reported it */
  component: string;
  /** human-readable, never parsed */
  detail: string;
}

/** ---------------------------------------------------------------- geometry */
export interface Vec3 {
  x: number;
  y: number;
  z: number;
}

export interface Quat {
  w: number;
  x: number;
  y: number;
  z: number;
}

/** Estimated during a build, never configured by hand: phone lenses are unknown. */
export interface Intrinsics {
  fx: number;
  fy: number;
  cx: number;
  cy: number;
  /** Brown-Conrady */
  k1: number;
  /** Brown-Conrady */
  k2: number;
  /** Brown-Conrady */
  k3: number;
  /** Brown-Conrady */
  p1: number;
  /** Brown-Conrady */
  p2: number;
  width: number;
  height: number;
  /** 0 == global shutter / unknown */
  rollingShutterLineTimeNs: number;
  estimated: boolean;
}

/** ---------------------------------------------------------------- sensing */
export interface ImuSample {
  timestampNs: number;
  /** rad/s */
  angularVelocity: Vec3;
  /** m/s^2 */
  acceleration: Vec3;
  hasMagnetometer: boolean;
  magneticField: Vec3;
  /**
   * Platforms that report a fused absolute orientation rather than raw rates fill these.
   * MotionCapability::OrientationOnly promises exactly that, and without somewhere to put it the
   * contract described a device it had no way to carry data from — which is what the browser
   * turns out to be: DeviceOrientation gives an orientation, not a rate.
   */
  hasOrientation: boolean;
  orientation: Quat;
}

export interface PoseSample {
  timestampNs: number;
  orientation: Quat;
  angularVelocity: Vec3;
  /** [0,1] */
  confidence: number;
  visuallyCorrected: boolean;
}

export type MotionCapability = 'None' | 'OrientationOnly' | 'GyroAccel' | 'GyroAccelMag';

/** ---------------------------------------------------------------- pixels */
export type PixelFormat = 'Unknown' | 'RGBA8' | 'BGRA8' | 'NV12' | 'I420' | 'Gray8' | 'EncodedJpeg';

export type Residency = 'HeapPinned' | 'HeapEncoded' | 'GpuTexture' | 'Spilled';

/**
 * A handle, not a buffer. Pixel bytes never cross the boundary as a value.
 * Deliberately carries no residency field: residency is store state, not frame identity. A copy
 * of this handle taken before a spill would otherwise claim the bytes are still in the heap.
 * Ask IFrameStoreAccess::ResidencyOf instead.
 */
export interface FrameRef {
  id: FrameId;
  buffer: BufferId;
  format: PixelFormat;
  width: number;
  height: number;
  stride: number;
  timestampNs: number;
  /** build-graph fingerprinting */
  contentHash: bigint;
}

/** ---------------------------------------------------------------- capture plan */
export type TessellationStrategy = 'Rings' | 'Geodesic' | 'Adaptive';

export interface CapturePlanSpec {
  strategy: TessellationStrategy;
  /** 0 => probe the camera */
  horizontalFovDeg: number;
  verticalFovDeg: number;
  /** fraction */
  overlapTarget: number;
  acceptanceConeDeg: number;
  coverPoles: boolean;
  motion: MotionCapability;
}

export interface CoverageNode {
  id: NodeId;
  targetOrientation: Quat;
  acceptanceConeDeg: number;
  ringIndex: number;
}

export interface CapturePlan {
  nodes: CoverageNode[];
  spec: CapturePlanSpec;
}

export type NodeState = 'Pending' | 'Capturing' | 'Captured' | 'Satisfied' | 'Flagged' | 'Retaking';

/** ---------------------------------------------------------------- candidates */
export interface QualityScore {
  /** higher is better */
  sharpness: number;
  /** estimated px of smear, lower is better */
  motionBlur: number;
  exposureAgreement: number;
  /** px, vs the cell's other candidates */
  alignmentResidual: number;
  /** from intra-cell disagreement */
  moverPenalty: number;
  /** the single number selection sorts on */
  aggregate: number;
}

export interface Candidate {
  id: CandidateId;
  node: NodeId;
  frame: FrameRef;
  pose: PoseSample;
  quality: QualityScore;
}

export interface BurstSpec {
  frameCount: number;
  intervalMs: number;
  lockExposure: boolean;
  lockWhiteBalance: boolean;
  lockFocus: boolean;
}

/** ---------------------------------------------------------------- guidance */
export type GuidanceAction = 'Seek' | 'HoldStill' | 'Firing' | 'CellDone' | 'SphereDone' | 'TooFast';

export interface CaptureGuidance {
  targetNode: NodeId;
  angularErrorDeg: number;
  rollErrorDeg: number;
  /** [0,1] */
  stability: number;
  action: GuidanceAction;
}

export interface CoverageState {
  nodesTotal: number;
  nodesSatisfied: number;
  coveredSolidAngleFraction: number;
  holes: NodeId[];
  underOverlapped: NodeId[];
}

/** ---------------------------------------------------------------- build */
export type Projection = 'Equirectangular' | 'Cubemap';

export type QualityTier = 'Preview' | 'Standard' | 'Maximum';

export interface BuildSpec {
  tier: QualityTier;
  projection: Projection;
  outputWidth: number;
  ghostAware: boolean;
}

export type BuildStage = 'Queued' | 'Features' | 'PairwiseMatching' | 'GlobalSolve' | 'ExposureCompensation' | 'GhostDetection' | 'SeamFinding' | 'Blending' | 'Projecting' | 'Complete' | 'Failed';

export interface BuildProgress {
  id: BuildId;
  stage: BuildStage;
  fraction: number;
  tilesReady: number;
  tilesTotal: number;
  failure: Status;
}

export interface GhostRegion {
  node: NodeId;
  centerAzimuthDeg: number;
  centerElevationDeg: number;
  radiusDeg: number;
  confidence: number;
}

export interface GhostReport {
  regions: GhostRegion[];
}

/** ---------------------------------------------------------------- export */
export type EncodeFormat = 'Jpeg' | 'Avif' | 'Png';

export interface EncodeSpec {
  format: EncodeFormat;
  quality: number;
  attachGPanoXmp: boolean;
  /** for the GPano metadata block */
  fullPanoWidth: number;
  /** for the GPano metadata block */
  fullPanoHeight: number;
}

export interface PanoramaRef {
  build: BuildId;
  projection: Projection;
  width: number;
  height: number;
  tileSize: number;
  tiles: FrameRef[];
  preview: FrameRef;
}

/**
 * --------------------------------------------------- engine value types
 * Data has no layer: these cross engine contracts freely, which is exactly why they
 * live here rather than in any one engine's header.
 */
export type PoseMode = 'Fused' | 'GyroOnly' | 'VisionOnly';

export interface SelectionPolicy {
  weightSharpness: number;
  weightMotionBlur: number;
  weightExposure: number;
  weightAlignment: number;
  weightMover: number;
  preferPoseAccuracy: boolean;
}

export interface NodeContext {
  targetOrientation: Quat;
  /** the rest of this cell's burst */
  siblings: Candidate[];
  /** selected candidates of adjacent cells */
  neighbours: Candidate[];
}

export interface FeatureSet {
  frame: FrameId;
  count: number;
  /** opaque, lives in the frame store */
  descriptors: BufferId;
  keypoints: BufferId;
}

export interface PairwiseResult {
  a: FrameId;
  b: FrameId;
  relativeRotation: Quat;
  inliers: number;
  medianResidualPx: number;
  accepted: boolean;
}

export interface GlobalSolution {
  frames: FrameId[];
  /** parallel to frames */
  rotations: Quat[];
  /** shared across frames, refined here */
  intrinsics: Intrinsics;
  medianResidualPx: number;
  droppedFrames: number;
}

export interface GainMap {
  perFrameGain: number[];
  frames: FrameId[];
}

export interface SeamMap {
  labelBuffer: BufferId;
  width: number;
  height: number;
}

/** ------------------------------------------------- platform value types */
export interface CameraCapabilities {
  maxWidth: number;
  maxHeight: number;
  /** 0 when the platform will not say */
  horizontalFovDeg: number;
  /** 0 when the platform will not say */
  verticalFovDeg: number;
  supportsExposureLock: boolean;
  supportsFocusLock: boolean;
  supportsTorch: boolean;
  maxBurstFps: number;
}

export interface CameraOpenSpec {
  preferredWidth: number;
  preferredHeight: number;
  preferRearCamera: boolean;
}

export interface FrameStoreBudget {
  /** measured at startup, not assumed */
  heapCeilingBytes: number;
  heapUsedBytes: number;
  spilledBytes: number;
}

export interface ComputeCapabilities {
  webgpu: boolean;
  simd: boolean;
  /** 0 == single-threaded; a supported mode, not a failure */
  threads: number;
  gpuMaxBufferBytes: number;
}

export type Kernel = 'WarpEquirect' | 'GaussianPyramid' | 'LaplacianPyramid' | 'MultibandBlend' | 'Downsample' | 'AbsDiffMask' | 'GainMap';

export interface KernelArgs {
  inputs: FrameRef[];
  outputs: FrameRef[];
  scalars: number[];
}

/**
 * The one place that knows whether work runs on the GPU, on threads, or serially.
 * Both backends must produce numerically equivalent results; a differential test enforces it.
 * -------------------------------------------------- manager value types
 */
export type FrameVerdict = 'Accepted' | 'RejectedQuality' | 'RejectedPose' | 'BurstComplete';

export interface ProjectSummary {
  id: ProjectId;
  title: string;
  createdAtMs: number;
  nodesTotal: number;
  nodesSatisfied: number;
  hasBuild: boolean;
}

export interface ExportSpec {
  encode: EncodeSpec;
  filename: string;
  /** share sheet if available, otherwise download */
  share: boolean;
}

/**
 * V1 — how a capture session is sequenced. Holds the live session: the plan, the per-cell
 * candidate sets, the current pose estimate.
 * It decides *when* to ask each engine, never *how*: it does not decide what "best" means (V6),
 * where a reticle sits (V4), or how bytes are stored (V11).
 */
export interface CaptureSessionManager {
  begin(project: ProjectId, spec: CapturePlanSpec): Promise<Result<SessionId>>;
  getPlan(): Promise<Result<CapturePlan>>;
  /** Called at sensor rate from the capture loop. Cheap by contract. */
  onMotion(samples: ImuSample[]): Promise<Result<CaptureGuidance>>;
  /** Fires a burst at the given cell and folds the results into its candidate set. */
  captureCell(node: NodeId, burst: BurstSpec): Promise<Result<Candidate[]>>;
  /** For externally sourced frames: file import, replayed datasets, manual shutter. */
  offerFrame(node: NodeId, frame: FrameRef, pose: PoseSample): Promise<Result<FrameVerdict>>;
  coverage(): Promise<Result<CoverageState>>;
  candidates(node: NodeId): Promise<Result<Candidate[]>>;
  /**
   * Re-arms a cell. Existing candidates are kept unless `replace` is set, so a retake can add to
   * the evidence pool rather than discard it.
   */
  requestRetake(node: NodeId, replace: boolean): Promise<Result<void>>;
  end(): Promise<Result<void>>;
}

/** V2 — how a panorama is built, including incremental rebuild. */
export interface PanoramaBuildManager {
  start(session: SessionId, spec: BuildSpec): Promise<Result<BuildId>>;
  poll(build: BuildId): Promise<Result<BuildProgress>>;
  panorama(build: BuildId): Promise<Result<PanoramaRef>>;
  ghosts(build: BuildId): Promise<Result<GhostReport>>;
  /**
   * The mechanism behind both retakes and manual candidate switching: recompute only the
   * transitive closure downstream of the changed cells (docs/04 §4.4). An incremental rebuild
   * must equal a full rebuild bit for bit — that invariant is the safety net under the feature.
   */
  invalidate(build: BuildId, dirty: NodeId[]): Promise<Result<void>>;
  cancel(build: BuildId): Promise<Result<void>>;
}

/** V3 — project lifecycle and export. The only manager that touches IExportAccess. */
export interface ProjectManager {
  list(): Promise<Result<ProjectSummary[]>>;
  create(title: string): Promise<Result<ProjectId>>;
  resume(project: ProjectId): Promise<Result<SessionId>>;
  delete(project: ProjectId): Promise<Result<void>>;
  /**
   * A manual override of automatic burst selection. Marks the node dirty for the next build, so
   * it takes exactly the same path as a retake.
   */
  setSelection(project: ProjectId, node: NodeId, candidate: CandidateId): Promise<Result<void>>;
  export(project: ProjectId, build: BuildId, spec: ExportSpec): Promise<Result<void>>;
}

/**
 * V9 — where camera frames come from. Exposes business verbs (CaptureBurst), not getUserMedia,
 * so that a folder of frames can stand in for a phone camera in manager tests.
 */
export interface CameraAccess {
  open(spec: CameraOpenSpec): Promise<Result<CameraCapabilities>>;
  startPreview(): Promise<Result<void>>;
  stopPreview(): Promise<Result<void>>;
  /** Latest preview frame, for pose correction and guidance. Borrowed: valid until the next call. */
  peekPreviewFrame(): Promise<Result<FrameRef>>;
  /** Frames land in the frame store; only handles come back. */
  captureBurst(burst: BurstSpec): Promise<Result<FrameRef[]>>;
  setLocks(exposure: boolean, whiteBalance: boolean, focus: boolean): Promise<Result<void>>;
  close(): Promise<Result<void>>;
}

/**
 * V10 — where motion data comes from. Reporting MotionCapability::None is a normal outcome, not
 * an error: iOS requires a user gesture and the user may decline.
 * through marshalled values, so its TypeScript adapter is written against the shared-heap
 * protocol rather than mirroring this signature. See ADR 0009.
 */
export interface MotionSensorAccess {
  capabilities(): Promise<Result<MotionCapability>>;
  start(requestedHz: number): Promise<Result<void>>;
  /** Copies out of the shared ring buffer; returns how many samples were written. */
  drain(out: ImuSample[]): Promise<Result<number>>;
  stop(): Promise<Result<void>>;
}

/**
 * V12 — where project metadata is persisted. Documents only, never pixels: the split is what
 * makes "resume after the browser killed the tab" a metadata read plus lazy pixel faulting.
 */
export interface ProjectStoreAccess {
  listProjects(): Promise<Result<ProjectId[]>>;
  readDocument(project: ProjectId, key: string): Promise<Result<string>>;
  writeDocument(project: ProjectId, key: string, value: string): Promise<Result<void>>;
  deleteProject(project: ProjectId): Promise<Result<void>>;
}

/**
 * V13 — how images are decoded, encoded and tagged. Owns the XMP GPano block, which is what
 * makes an exported file open as a sphere rather than a wide photo.
 */
export interface ImageCodecAccess {
  decode(bytes: Uint8Array): Promise<Result<FrameRef>>;
  encode(frame: FrameRef, spec: EncodeSpec): Promise<Result<Uint8Array>>;
}

/** V15 — how a result leaves the device. */
export interface ExportAccess {
  save(filename: string, mimeType: string, bytes: Uint8Array): Promise<Result<void>>;
  canShare(): Promise<Result<boolean>>;
  share(filename: string, mimeType: string, bytes: Uint8Array): Promise<Result<void>>;
}
