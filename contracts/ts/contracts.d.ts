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
  /**
   * Whether `angularVelocity` is a measurement. It is the same distinction `hasOrientation` draws
   * and for the same reason: zero is a real rate, so a zeroed field cannot say whether the device
   * is still or the platform never looked. Reading "not measured" as "not moving" is how a phone
   * mid-swing was once reported as perfectly still (ADR 0025).
   * A platform that reports a fused attitude and no gyroscope leaves this false, and so does one
   * whose DeviceMotionEvent fires with a null rotationRate — which some desktops do.
   */
  hasAngularVelocity: boolean;
}

export interface PoseSample {
  timestampNs: number;
  orientation: Quat;
  angularVelocity: Vec3;
  /**
   * How much the orientation above is worth, in [0,1].
   * **Zero means no reading has ever anchored it**, which is not the same as "nothing has moved
   * it": integrating a gyroscope's rates turns the orientation degrees away from where it started
   * and says nothing whatever about where that was. A stream of rates alone reports zero for its
   * whole life, however far it has turned — measured at 8.709° off the identity, still zero — and
   * that is the honest answer, because the direction it is 8.709° away from is one nobody chose.
   * Callers act on the zero rather than on the number's size: `Locate` prefers the cell the camera
   * is inside only for an anchored pose, and names no cell as held otherwise (ADR 0041).
   * `ArmBurst` used to stand its cone check down on a zero and no longer does — there is nothing
   * to check against an identity nobody chose, so there is nothing to allow (ADR 0044). Above
   * zero, 1.0 is an absolute reading and 0.5 is dead reckoning
   * *from* one — drifting away from a direction somebody measured, which is worth aiming with and
   * an unanchored integration is not.
   * The old gloss said zero meant nothing had moved the orientation. An engine written against it
   * reports 0.5 for a heading nobody measured, and every rule above then fires on it: zero of
   * thirty-two cells armable on the shipped tessellation, with the client in aimed mode so nothing
   * on screen says why. This sentence is what a second `IPoseEngine` is written against, so it is
   * the sentence that has to be right.
   */
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

/**
 * A frame reduced to something a screen can take — and the one place pixel bytes are a value in
 * a contract rather than a handle.
 * The rule they are an exception to is a rule about cost. A `FrameRef` exists so that a
 * 1280x960 RGBA frame — 4.9 MB, and eight of them in a cell — is never serialised by value; a
 * review strip handed the frames themselves would move 39 MB across the boundary and hold it in
 * the page, against a browser heap ceiling of 128 MB (ADR 0023). The reduction is what pays that
 * bill: at a long edge of 128 this is 48 KB, three orders of magnitude down, and the reason for
 * the handle no longer applies. What a handle *cannot* do is the thing needed here — the page has
 * no frame store to resolve one against, and `IFrameStoreAccess::Pin` reaches no further than the
 * core (ADR 0038).
 * Always `RGBA8` and tightly packed, so the stride is `width * 4` and there is nothing to decode:
 * a browser can hand these straight to `ImageData` and a canvas.
 */
export interface FramePreview {
  frame: FrameId;
  width: number;
  height: number;
  format: PixelFormat;
  pixels: Uint8Array;
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
  /**
   * How long the camera is given to converge after the locks go on, before the first frame is
   * taken. It is a different quantity from the interval — that is how far apart two frames have
   * to be, this is how long one camera takes to settle — and a caller that knows its device
   * should be able to say so.
   * The default is a guess, and it is worth saying so plainly: nobody has measured how long a
   * phone camera takes to converge after a lock. What is known is one device in one scene. A
   * Pixel that holds a focus lock scored its five-frame burst 5.9, 1145, 720, 583, 586 in capture
   * order — the first frame taken 16 ms after arming and roughly 100x less sharp than any of its
   * siblings, the second taken 96 ms after arming and normal. 150 ms is that datapoint with
   * margin. Measuring it per device class is what would replace it.
   */
  settleMs: number;
  lockExposure: boolean;
  lockWhiteBalance: boolean;
  lockFocus: boolean;
}

/**
 * ---------------------------------------------------------------- guidance
 * What the user should do about the cell guidance is naming.
 * `CellDone` is an *edge*: the manager emits it on the one tick a burst fills, and callers act on
 * it once — **unless that tick fails**. Releasing the camera's locks is the last thing a filled
 * burst does, and a track that refuses returns that failure from `OnMotion`, so the cell is
 * committed and no action announces it. The failure winning is deliberate (a camera left locked is
 * the worse problem), which makes this a caller's problem: anything mirroring coverage off
 * `CellDone` has to re-read it on a failed tick too. `AlreadyCaptured` is a *level*: the camera is resting inside the cone of a cell that
 * already holds a capture, and it is true on every tick the phone stays there. They were briefly
 * the same value, which turned a once-per-cell refresh into one per animation frame.
 * Appended rather than inserted: the wire carries the index.
 */
export type GuidanceAction = 'Seek' | 'HoldStill' | 'Firing' | 'CellDone' | 'SphereDone' | 'TooFast' | 'AlreadyCaptured' | 'Fire';

export interface CaptureGuidance {
  targetNode: NodeId;
  angularErrorDeg: number;
  rollErrorDeg: number;
  /** [0,1] */
  stability: number;
  action: GuidanceAction;
  /**
   * Whether the orientation this answer was computed from was a measurement at all.
   * It is here because a client has to know which of two answers it is reading and has no other
   * way to find out. With no aim, `angularErrorDeg` is measured from an identity nobody chose and
   * comes back near zero for whichever cell happens to sit there — so a page that drew its
   * reticle from it would show a closed ring on a pose nothing had measured, and a horizon rolled
   * against nothing. It parks both instead.
   * Guessing from "the sensor started" is not the same fact: it is wrong for every tick before
   * the first sample arrives, which is what turned twelve browser tests red when the page tried.
   * It used to gate a shutter as well, on a device that captured without an aim at all. That
   * device is refused now (ADR 0044) and the shutter is gone with it; this field is presentation.
   * Appended rather than inserted, because field order is wire order.
   */
  aimKnown: boolean;
  /**
   * How much of the dwell the camera has served on this cell, in [0,1]. One at the moment `action`
   * is `Fire`, zero whenever there is nothing to hold on.
   * Published so the ring the user watches and the trigger that fires are **the same number in the
   * same message**. A client counting its own dwell would be a second copy of a fact this manager
   * already holds, and the two would disagree exactly when it mattered — a progress bar that
   * filled and did not fire, or fired before it filled.
   * The dwell is counted here rather than in a client for a reason a client cannot work around:
   * `performance.now()` keeps moving when the sensor stops delivering, so a page counting elapsed
   * time matures its dwell on guidance about a cell the phone may have left. This manager has the
   * sample timestamps and `IClock` side by side, and declines to accumulate over an interval no
   * pose arrived in (ADR 0043).
   * Appended, for the same reason as the field above.
   */
  heldFraction: number;
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

/**
 * Everything a pose estimate carries from one batch of samples to the next.
 * It exists so that IPoseEngine can be a pure function of it. An engine that kept this in members
 * would be a stateful engine, which rule 4 in docs/03 §3.3 forbids for a reason worth more than
 * the convenience: an estimate you cannot construct is an estimate you cannot replay against a
 * recorded log, and replay is how a fusion filter is judged (ADR 0016).
 * CaptureSessionManager owns one per session, which is where session state belongs.
 */
export interface PoseState {
  mode: PoseMode;
  capability: MotionCapability;
  pose: PoseSample;
  /**
   * Whether any sample has arrived at all. It is what the elapsed time between samples is measured
   * from, so the first sample of a stream sets it whether or not it moved anything — a rate has no
   * orientation in it until there is an interval to integrate it over.
   * It is *not* the answer to "is this orientation a measurement": that is `anchored` below, and
   * `PoseSample.confidence` is what callers should read. The two were one flag, and the first
   * rate-only sample of a stream then reported an integrated pose before anything was integrated.
   */
  observed: boolean;
  /**
   * Whether this orientation descends from an absolute reading — not whether something moved it.
   * Set the first time an attitude is folded in and never cleared, so it survives the stretches
   * where `absolute` goes false: dead reckoning after a reading is still an estimate *of a
   * direction somebody measured*, which is what confidence 0.5 means.
   * The distinction is the whole of ADR 0042 and it is not the one this field was first written
   * with. It used to mean "something moved the orientation", which dead reckoning also does — so a
   * gyroscope-only stream integrating away from the identity it was born with reported confidence
   * 0.5 for a heading nobody had ever measured, and `ArmBurst` then enforced the acceptance cone
   * against it: zero of thirty-two cells armable on the shipped tessellation, which is ADR 0042's
   * own failure reached through the other door. A rate says how fast the device is turning and
   * nothing about where it started.
   * Callers act on this through `PoseSample.confidence`, which is derived from it: `Locate`
   * prefers aim only for an anchored pose (ADR 0041). What that buys is no longer a sensorless
   * capture — that is refused (ADR 0044) — but a session whose first ticks, and whose rate-only
   * streams, cannot be mistaken for an aim and fire a burst at a cell nobody pointed at.
   */
  anchored: boolean;
  /**
   * Whether the pose came from an absolute reading rather than from integrating rates. Confidence
   * is derived from this, so it has to survive between calls.
   */
  absolute: boolean;
  /**
   * The gyroscope's zero offset, in rad/s, as far as the engine has been able to observe it. A
   * gyroscope at rest does not read zero, and integrating that offset is how dead reckoning walks
   * away from the truth during the seconds when no absolute reading is arriving to correct it.
   * It is here rather than inside the engine because it is session state, and an engine is
   * stateless per session (docs/03 §3.3 rule 4, ADR 0016). A bias kept in the engine would be
   * shared by every session in the process and would outlive the device being put down.
   */
  gyroBias: Vec3;
}

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
  /**
   * The mode the camera actually settled on, not the largest it could reach. The coverage plan is
   * sized from these, so they have to describe the frames that will arrive: a sensor maximum the
   * preview never runs at would derive an aspect ratio, and so a ring count, for a frame nobody
   * captures. What the caller asks for is CameraOpenSpec's business; this is the answer.
   */
  maxWidth: number;
  /**
   * The mode the camera actually settled on, not the largest it could reach. The coverage plan is
   * sized from these, so they have to describe the frames that will arrive: a sensor maximum the
   * preview never runs at would derive an aspect ratio, and so a ring count, for a frame nobody
   * captures. What the caller asks for is CameraOpenSpec's business; this is the answer.
   */
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
  /**
   * Whether this project has a capture session written down — something to come back to.
   * The fact, not a promise: `ICaptureSessionManager::Resume` is still free to refuse this
   * document, and a client that treated the flag as a guarantee would have nowhere to put that
   * refusal. What it buys is that a page never has to *try* a resume to find out whether to
   * offer one — a successful attempt opens the camera and starts tracking, so probing commits to
   * a resume nobody asked for (ADR 0036).
   */
  hasSession: boolean;
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
  /**
   * Opens a capture session on a project that already exists.
   * Refused with `SensorUnavailable` when `IMotionSensorAccess::Capabilities()` reports `None` or
   * cannot answer, and refused *before* a camera is opened. A capture places every frame by the
   * direction the phone was pointing, and a device that cannot sense one produces cells labelled
   * with directions nobody measured — a failure invisible until a build, so it is refused at the
   * door instead (ADR 0044). A client whose own platform can answer that question earlier should:
   * this call is the rule, not the only place to be polite about it.
   * Refused with `NotFound` when the project does not exist, which is checked first: beginning
   * against an id nobody created would leave a titleless project in the user's list.
   */
  begin(project: ProjectId, spec: CapturePlanSpec): Promise<Result<SessionId>>;
  /**
   * Picks a session back up from what was written down about it.
   * A tab that goes away mid-capture takes the plan, the candidate sets and the frame store with
   * it, and the phone that comes back is the same phone standing in the same spot — so the cells
   * already captured have to still count. What survives is the project store's documents and
   * whatever the frame store's sink is holding; this reads the first and hands the frames it
   * names back to the store, which is why a resumed candidate can still be pinned.
   * The plan is the stored one rather than a fresh tessellation. Node ids are indices into a
   * particular sphere, so replanning from whatever lens is in front of the phone now would file
   * every restored candidate under a different cell.
   * The motion capability is not stored, and this reads the live one: a document says which
   * sphere is being captured, never what the device it comes back on can sense. So this is
   * refused with `SensorUnavailable` on exactly the terms `Begin` is, and on the same phone that
   * began the capture if the user declined the permission this time (ADR 0044).
   */
  resume(project: ProjectId): Promise<Result<SessionId>>;
  getPlan(): Promise<Result<CapturePlan>>;
  /**
   * The session's tick, called at sensor rate from the capture loop. Cheap by contract.
   * It also advances an armed burst by at most one frame, because this is the only call the
   * client makes often enough to pace one: a burst takes time, and time is something a
   * synchronous port cannot wait for (ADR 0018). Guidance reports `Firing` until the burst is
   * full and `CellDone` on the tick that fills it.
   */
  onMotion(samples: ImuSample[]): Promise<Result<CaptureGuidance>>;
  /**
   * Arms a burst at the given cell. It does not fire one: the frames arrive over the following
   * ticks, and the candidates are readable through Candidates(node) once guidance says CellDone.
   * `burst.intervalMs` is a floor rather than a cadence — at most one frame is taken per tick, so
   * a spec asking for less than a tick apart gets a tick apart, and a spec asking for less than
   * the camera's own `maxBurstFps` period gets that instead. Locks are applied here and held
   * until the burst completes or is abandoned.
   * The first frame is not taken until `burst.settleMs` after arming, because the locks applied
   * on this call are what the camera has to converge to. Under that floor the camera's own frame
   * period applies as well: `PeekPreviewFrame` borrows the latest preview frame, and inside one
   * frame period the latest frame is one the camera produced before the locks landed.
   * Refused with `FailedPrecondition` when the camera is not aimed at the cell — outside the
   * acceptance cone the plan gave it, which is the same cone guidance closes its reticle on. A
   * burst records whatever the camera is looking at and the node is only a name to file it under,
   * so arming against a cell somewhere else stores a good picture in the wrong place: sharp, well
   * scored, and undetectable afterwards (ADR 0041). The caller fixes it by turning the phone.
   * **Unconditionally, and this paragraph used to say the opposite.** Until ADR 0044 the check
   * stood down whenever `PoseSample.confidence` was zero, so that a phone with no motion sensor
   * could reach every cell of its own plan by eye. Such a phone is now refused at `Begin`, and
   * what is left of zero confidence is transient: the ticks before a session's first reading, and
   * a stream carrying rates with no attitude in them. In both the pose is the identity it was
   * born with, which is a direction nobody chose, so there is nothing to check and nothing to
   * allow — arming then would file real pixels under a cell picked by an accident of
   * initialisation.
   * So a client may wait for guidance to say `HoldStill`, which means "inside this cell's cone
   * and this cell still wants shooting" — precisely the condition this arms on. What it must not
   * assume is that the action always comes: it needs an anchored pose, so a stream that carries
   * rates and never an attitude produces a session that begins and can never arm. The shipped
   * browser adapter cannot produce one (every sample it emits carries an attitude), and a port
   * that can owes its user a way to say so. Nothing in this build watches for it; the roadmap
   * carries it.
   */
  armBurst(node: NodeId, burst: BurstSpec): Promise<Result<void>>;
  /** For externally sourced frames: file import, replayed datasets, manual shutter. */
  offerFrame(node: NodeId, frame: FrameRef, pose: PoseSample): Promise<Result<FrameVerdict>>;
  coverage(): Promise<Result<CoverageState>>;
  /**
   * Ranked best-first, by the same `IFrameQualityEngine::Rank` the manager already asks on every
   * committed burst. The order is an answer rather than a record of when the shutter fired, so a
   * review client can show a strip and name the automatic pick without deciding what "best"
   * means — which is V6's, and not a client's to borrow.
   */
  candidates(node: NodeId): Promise<Result<Candidate[]>>;
  /**
   * One candidate's frame, reduced to something a screen can take.
   * The counterpart of `Candidates`, and the reason it is here rather than anywhere else: that
   * call hands back what the core *knows* about a candidate, and a person choosing between five
   * frames of the same wall needs to see them. A `FrameRef` cannot do that job — the page has no
   * frame store to resolve one against, and `IFrameStoreAccess::Pin` reaches no further than the
   * core — so this is the one call in the contracts that answers with pixels (ADR 0038).
   * `maxEdge` bounds the long edge and the caller states it, because how large a thumbnail wants
   * to be is a fact about the screen it is going on. It is bounded in turn: past
   * `kFramePreviewMaxEdge` the reduction stops paying for itself and the call is refused.
   * `NotFound` covers both halves of a stale request — a cell that is not in the plan, and a
   * candidate this cell no longer holds. A replace-retake forgets a cell's frames, so a client
   * showing a strip it fetched a moment ago can ask about a candidate that has since gone; that
   * is an ordinary answer here rather than a fault.
   * Reading a preview does not warm a cell. A captured cell's frames have been cooled to whatever
   * cheaper tier the store has (ADR 0023) and faulting one in to look at it would leave it
   * resident — eight candidates of a 1280x960 frame are 39 MB, so a user opening three cells
   * would fill a phone's heap by browsing. Whatever residency a frame had before this call, it
   * has after it: the tier is read first and restored by name, so a store with tiers this build
   * has never seen gets its frame back where it had it. Best effort, and deliberately so — a
   * store that will not take the frame back leaves it readable in the heap, which is a worse
   * ceiling rather than a lost frame.
   */
  candidatePreview(node: NodeId, candidate: CandidateId, maxEdge: number): Promise<Result<FramePreview>>;
  /**
   * Re-arms a cell. Existing candidates are kept unless `replace` is set, so a retake can add to
   * the evidence pool rather than discard it.
   * "Re-arms" is about the cell's state, not about a burst: the burst that follows still goes
   * through `ArmBurst` and is still refused if the camera is not aimed at the cell (ADR 0041). So
   * a retake asks the user to point at the cell again before anything is recorded — which is the
   * point, since a retake that captured from wherever the phone happened to be pointing is the bug
   * ADR 0041 exists to stop. `docs/03-architecture.md` UC-2 describes the flow.
   * **Without exemption, and this paragraph used to carry one.** It said the burst after a retake
   * arms wherever the phone is pointing when the pose was never anchored, because that was the
   * only way a sensorless device could retake at all. That device is refused at `Begin` now
   * (ADR 0044), so the rule above is the whole rule: point at the cell again, and the burst is
   * taken there or not at all.
   * With `replace` false, that burst has nothing to fire it in this build, and the honest place
   * to say so is here. Keeping the evidence leaves the cell covered, `Locate` answers
   * `AlreadyCaptured` rather than `HoldStill` for a covered cell, and the dwell that arms every
   * burst since ADR 0043 only matures on `HoldStill` — so an additive retake marks nothing a
   * client can act on. `replace` true empties the cell of everything the store will let go of,
   * which makes it a hole again and puts it back in the dwell's way — everything, unless the
   * store refuses to forget a frame, in which case that one candidate stays and the cell stays
   * covered until a later retake succeeds. `Discard` keeps it deliberately: the bytes are still
   * charged, and dropping the last handle to them would orphan them. The retake flow that closes
   * the additive case is Phase 3 (`docs/06-roadmap.md`); until then this call aborts a burst in
   * flight and, additively, does nothing else.
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
  /**
   * No Resume here, deliberately. Picking a capture back up means handing a live session to
   * whatever owns session state, and that is `ICaptureSessionManager` — this manager could only
   * ever have returned a SessionId it had no way to make (ADR 0029).
   * `hasSession` is not that method coming back. It reports that a session document exists, which
   * is metadata about a project and nothing a caller could mistake for a session: there is no
   * SessionId in a summary, and nothing here parses the document or hands back what is in it. It
   * is what lets a page offer a resume rather than discover one by attempting it (ADR 0036).
   */
  list(): Promise<Result<ProjectSummary[]>>;
  create(title: string): Promise<Result<ProjectId>>;
  delete(project: ProjectId): Promise<Result<void>>;
  /**
   * A manual override of automatic burst selection. Marks the node dirty for the next build, so
   * it takes exactly the same path as a retake.
   * An unset cell or candidate is refused. Zero is what `GetSelection` answers for "nobody has
   * chosen here", so writing one would put the two halves of this pair in contradiction: a
   * document the writer accepted and the reader has to call corrupt.
   */
  setSelection(project: ProjectId, node: NodeId, candidate: CandidateId): Promise<Result<void>>;
  /**
   * What was chosen for a cell, or nothing.
   * The counterpart of `SetSelection`, and it exists because nothing else could answer: a pick was
   * written here and read nowhere, so a review client's only way to show which candidate was in
   * force was to remember its own writes — which a reload forgets, along with the choice the user
   * had just made.
   * **A zero candidate means nobody has chosen here, and it is a success.** `Id::valid()` is
   * `value != 0` and every counter in these contracts starts at 1, so zero is a value no selection
   * can have. It is deliberately not `NotFound`: a caller reads a Result's status to tell a call
   * that failed from one that worked, and folding "no override" into the failure branch would make
   * a project this build cannot read look exactly like one nobody has edited. A project that does
   * not exist is still a failure, because that is a question about the project rather than an
   * answer about the cell — and so is an unset cell, which nothing can have written a selection
   * for. The sentinel only means something because every other answer is a real one.
   */
  getSelection(project: ProjectId, node: NodeId): Promise<Result<CandidateId>>;
  export(project: ProjectId, build: BuildId, spec: ExportSpec): Promise<Result<void>>;
}

/**
 * V9 — where camera frames come from. Exposes a lens and a latest frame, not getUserMedia, so
 * that a folder of frames can stand in for a phone camera in manager tests.
 * Every call here is a read or a write of state the device already holds, which is what lets the
 * port stay synchronous over a resident host (ADR 0014). There is deliberately no burst verb: a
 * burst takes time, and the one call that took time is the one that could not be implemented.
 * CaptureSessionManager paces a burst over PeekPreviewFrame instead, one frame per tick of the
 * loop the client is already running (ADR 0018).
 */
export interface CameraAccess {
  open(spec: CameraOpenSpec): Promise<Result<CameraCapabilities>>;
  startPreview(): Promise<Result<void>>;
  stopPreview(): Promise<Result<void>>;
  /**
   * The latest frame, in the frame store; only a handle comes back. This is the whole pixel path:
   * pose correction, guidance and every frame of every burst arrive through here.
   * **Each call allocates a frame and hands it over.** Not a borrow: the caller owns what comes
   * back and is the one that must Forget it. CaptureSessionManager keeps peeked frames as a
   * burst's candidates and forgets them when the burst rolls back, so a port that returned the
   * same handle twice — or one it later reclaimed — would have the manager scoring a frame whose
   * bytes had been overwritten, then forgetting a frame twice. Repeated peeks therefore return
   * distinct frames, which the contract suite asserts of every implementation.
   * It follows that an implementation reaches IFrameStoreAccess, since a FrameRef is a handle
   * into it and there is nowhere else for one to come from. That is a port depending on a port,
   * which the layer rules otherwise forbid; ADR 0021 records why the frame store is the
   * exception rather than each camera being one.
   */
  peekPreviewFrame(): Promise<Result<FrameRef>>;
  setLocks(exposure: boolean, whiteBalance: boolean, focus: boolean): Promise<Result<void>>;
  close(): Promise<Result<void>>;
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
