/**
 * TypeScript mirror of the subset of contracts that crosses the WASM/JS boundary.
 *
 * Two directions:
 *   - Manager facades: the PWA clients call into the core.
 *   - ResourceAccess ports: the core calls back out into browser adapters.
 *
 * Hand-mirrored today; generated from the C++ headers from Phase 0 onward, with a CI check that
 * fails on drift. Do not add anything here that has no counterpart in contracts/cpp.
 */

export type SessionId = number & { readonly __brand: 'SessionId' };
export type ProjectId = number & { readonly __brand: 'ProjectId' };
export type NodeId = number & { readonly __brand: 'NodeId' };
export type FrameId = number & { readonly __brand: 'FrameId' };
export type CandidateId = number & { readonly __brand: 'CandidateId' };
export type BuildId = number & { readonly __brand: 'BuildId' };
export type BufferId = number & { readonly __brand: 'BufferId' };

export type StatusCode =
  | 'Ok' | 'InvalidArgument' | 'NotFound' | 'FailedPrecondition' | 'Cancelled' | 'Unsupported'
  | 'SensorPermissionDenied' | 'SensorUnavailable' | 'CameraUnavailable'
  | 'FrameStoreExhausted' | 'StorageQuotaExceeded' | 'ComputeUnavailable'
  | 'RegistrationFailed' | 'InsufficientCoverage' | 'CodecFailure' | 'Internal';

export interface Status { code: StatusCode; component: string; detail: string; }
export type Result<T> = { ok: true; value: T } | { ok: false; status: Status };

export interface Quat { w: number; x: number; y: number; z: number; }
export interface Vec3 { x: number; y: number; z: number; }

export interface ImuSample {
  timestampNs: number;
  angularVelocity: Vec3;
  acceleration: Vec3;
  hasMagnetometer: boolean;
  magneticField: Vec3;
}

export interface PoseSample {
  timestampNs: number;
  orientation: Quat;
  angularVelocity: Vec3;
  confidence: number;
  visuallyCorrected: boolean;
}

export type MotionCapability = 'None' | 'OrientationOnly' | 'GyroAccel' | 'GyroAccelMag';
export type PixelFormat = 'Unknown' | 'RGBA8' | 'BGRA8' | 'NV12' | 'I420' | 'Gray8' | 'EncodedJpeg';
export type Residency = 'HeapPinned' | 'HeapEncoded' | 'GpuTexture' | 'Spilled';

/** A handle. Pixel bytes never travel through postMessage. */
export interface FrameRef {
  id: FrameId;
  buffer: BufferId;
  format: PixelFormat;
  residency: Residency;
  width: number; height: number; stride: number;
  timestampNs: number;
  contentHash: bigint;
}

export type GuidanceAction = 'Seek' | 'HoldStill' | 'Firing' | 'CellDone' | 'SphereDone' | 'TooFast';

export interface CaptureGuidance {
  targetNode: NodeId;
  angularErrorDeg: number;
  rollErrorDeg: number;
  stability: number;
  action: GuidanceAction;
}

export interface CoverageNode { id: NodeId; targetOrientation: Quat; acceptanceConeDeg: number; ringIndex: number; }
export interface CoverageState {
  nodesTotal: number; nodesSatisfied: number;
  coveredSolidAngleFraction: number;
  holes: NodeId[]; underOverlapped: NodeId[];
}

export interface QualityScore {
  sharpness: number; motionBlur: number; exposureAgreement: number;
  alignmentResidual: number; moverPenalty: number; aggregate: number;
}

export interface Candidate { id: CandidateId; node: NodeId; frame: FrameRef; pose: PoseSample; quality: QualityScore; }
export interface BurstSpec { frameCount: number; intervalMs: number; lockExposure: boolean; lockWhiteBalance: boolean; lockFocus: boolean; }

export interface CapturePlanSpec {
  strategy: 'Rings' | 'Geodesic' | 'Adaptive';
  horizontalFovDeg: number; verticalFovDeg: number;
  overlapTarget: number; acceptanceConeDeg: number;
  coverPoles: boolean; motion: MotionCapability;
}
export interface CapturePlan { nodes: CoverageNode[]; spec: CapturePlanSpec; }

export type BuildStage =
  | 'Queued' | 'Features' | 'PairwiseMatching' | 'GlobalSolve' | 'ExposureCompensation'
  | 'GhostDetection' | 'SeamFinding' | 'Blending' | 'Projecting' | 'Complete' | 'Failed';

export interface BuildSpec {
  tier: 'Preview' | 'Standard' | 'Maximum';
  projection: 'Equirectangular' | 'Cubemap';
  outputWidth: number;
  ghostAware: boolean;
}
export interface BuildProgress {
  id: BuildId; stage: BuildStage; fraction: number;
  tilesReady: number; tilesTotal: number; failure: Status;
}
export interface GhostRegion { node: NodeId; centerAzimuthDeg: number; centerElevationDeg: number; radiusDeg: number; confidence: number; }
export interface GhostReport { regions: GhostRegion[]; }
export interface PanoramaRef {
  build: BuildId;
  projection: 'Equirectangular' | 'Cubemap';
  width: number; height: number; tileSize: number;
  tiles: FrameRef[]; preview: FrameRef;
}
export interface ProjectSummary {
  id: ProjectId; title: string; createdAtMs: number;
  nodesTotal: number; nodesSatisfied: number; hasBuild: boolean;
}
export interface EncodeSpec { format: 'Jpeg' | 'Avif' | 'Png'; quality: number; attachGPanoXmp: boolean; fullPanoWidth: number; fullPanoHeight: number; }
export interface ExportSpec { encode: EncodeSpec; filename: string; share: boolean; }

/* ------------------------------------------------------------------ managers */
/* The clients' entire surface. Async because they are proxied to the core worker. */

export type FrameVerdict = 'Accepted' | 'RejectedQuality' | 'RejectedPose' | 'BurstComplete';

export interface CaptureSessionManager {
  begin(project: ProjectId, spec: CapturePlanSpec): Promise<Result<SessionId>>;
  getPlan(): Promise<Result<CapturePlan>>;
  onMotion(samples: ImuSample[]): Promise<Result<CaptureGuidance>>;
  captureCell(node: NodeId, burst: BurstSpec): Promise<Result<Candidate[]>>;
  coverage(): Promise<Result<CoverageState>>;
  candidates(node: NodeId): Promise<Result<Candidate[]>>;
  requestRetake(node: NodeId, replace: boolean): Promise<Result<void>>;
  end(): Promise<Result<void>>;
}

export interface PanoramaBuildManager {
  start(session: SessionId, spec: BuildSpec): Promise<Result<BuildId>>;
  poll(build: BuildId): Promise<Result<BuildProgress>>;
  result(build: BuildId): Promise<Result<PanoramaRef>>;
  ghosts(build: BuildId): Promise<Result<GhostReport>>;
  invalidate(build: BuildId, dirty: NodeId[]): Promise<Result<void>>;
  cancel(build: BuildId): Promise<Result<void>>;
}

export interface ProjectManager {
  list(): Promise<Result<ProjectSummary[]>>;
  create(title: string): Promise<Result<ProjectId>>;
  resume(project: ProjectId): Promise<Result<SessionId>>;
  delete(project: ProjectId): Promise<Result<void>>;
  setSelection(project: ProjectId, node: NodeId, candidate: CandidateId): Promise<Result<void>>;
  export(project: ProjectId, build: BuildId, spec: ExportSpec): Promise<Result<void>>;
}

/* -------------------------------------------------- resource-access adapters */
/* Implemented in TypeScript, injected into the core at startup. The core calls these. */

export interface CameraCapabilities {
  maxWidth: number; maxHeight: number;
  horizontalFovDeg: number; verticalFovDeg: number;
  supportsExposureLock: boolean; supportsFocusLock: boolean; supportsTorch: boolean;
  maxBurstFps: number;
}

export interface CameraAccessAdapter {
  open(spec: { preferredWidth: number; preferredHeight: number; preferRearCamera: boolean }): Promise<Result<CameraCapabilities>>;
  startPreview(): Promise<Result<void>>;
  stopPreview(): Promise<Result<void>>;
  peekPreviewFrame(): Promise<Result<FrameRef>>;
  captureBurst(spec: BurstSpec): Promise<Result<FrameRef[]>>;
  setLocks(exposure: boolean, whiteBalance: boolean, focus: boolean): Promise<Result<void>>;
  close(): Promise<Result<void>>;
}

export interface MotionSensorAccessAdapter {
  capabilities(): Promise<Result<MotionCapability>>;
  start(requestedHz: number): Promise<Result<void>>;
  /** Writes into the shared ring buffer the core reads; returns the sample count written. */
  drain(): Promise<Result<number>>;
  stop(): Promise<Result<void>>;
}

export interface ProjectStoreAccessAdapter {
  listProjects(): Promise<Result<ProjectId[]>>;
  readDocument(project: ProjectId, key: string): Promise<Result<string>>;
  writeDocument(project: ProjectId, key: string, value: string): Promise<Result<void>>;
  deleteProject(project: ProjectId): Promise<Result<void>>;
}

export interface ExportAccessAdapter {
  save(filename: string, mimeType: string, bytes: Uint8Array): Promise<Result<void>>;
  canShare(): Promise<Result<boolean>>;
  share(filename: string, mimeType: string, bytes: Uint8Array): Promise<Result<void>>;
}

export interface CoreBootstrap {
  camera: CameraAccessAdapter;
  motion: MotionSensorAccessAdapter;
  projectStore: ProjectStoreAccessAdapter;
  export: ExportAccessAdapter;
  /** Shared heap view used for zero-copy frame transfer; absent when SAB is unavailable. */
  sharedHeap?: SharedArrayBuffer;
}
