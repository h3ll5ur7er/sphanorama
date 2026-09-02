/**
 * GENERATED FILE — DO NOT EDIT. Produced by tools/contract_gen.py.
 *
 * The TypeScript half of the boundary codec. Emitted from the same parse as the C++ half, so
 * field order and widths cannot drift apart (ADR 0013).
 */
import type * as C from '../../../contracts/ts/contracts';
import { Reader, Writer } from './wire';

export const StatusCodeValues: C.StatusCode[] = ['Ok', 'InvalidArgument', 'NotFound', 'FailedPrecondition', 'Cancelled', 'Unsupported', 'SensorPermissionDenied', 'SensorUnavailable', 'CameraUnavailable', 'FrameStoreExhausted', 'StorageQuotaExceeded', 'ComputeUnavailable', 'RegistrationFailed', 'InsufficientCoverage', 'CodecFailure', 'Internal'];
export const MotionCapabilityValues: C.MotionCapability[] = ['None', 'OrientationOnly', 'GyroAccel', 'GyroAccelMag'];
export const PixelFormatValues: C.PixelFormat[] = ['Unknown', 'RGBA8', 'BGRA8', 'NV12', 'I420', 'Gray8', 'EncodedJpeg'];
export const TessellationStrategyValues: C.TessellationStrategy[] = ['Rings', 'Geodesic', 'Adaptive'];
export const GuidanceActionValues: C.GuidanceAction[] = ['Seek', 'HoldStill', 'Firing', 'CellDone', 'SphereDone', 'TooFast'];
export const ProjectionValues: C.Projection[] = ['Equirectangular', 'Cubemap'];
export const QualityTierValues: C.QualityTier[] = ['Preview', 'Standard', 'Maximum'];
export const BuildStageValues: C.BuildStage[] = ['Queued', 'Features', 'PairwiseMatching', 'GlobalSolve', 'ExposureCompensation', 'GhostDetection', 'SeamFinding', 'Blending', 'Projecting', 'Complete', 'Failed'];
export const EncodeFormatValues: C.EncodeFormat[] = ['Jpeg', 'Avif', 'Png'];
export const FrameVerdictValues: C.FrameVerdict[] = ['Accepted', 'RejectedQuality', 'RejectedPose', 'BurstComplete'];

export function encodeStatus(out: Writer, value: C.Status): void {
  out.i32(StatusCodeValues.indexOf(value.code));
  out.string(value.component);
  out.string(value.detail);
}

export function decodeStatus(input: Reader): C.Status {
  return {
    code: StatusCodeValues[input.i32()],
    component: input.string(),
    detail: input.string(),
  };
}

export function encodeVec3(out: Writer, value: C.Vec3): void {
  out.f64(value.x);
  out.f64(value.y);
  out.f64(value.z);
}

export function decodeVec3(input: Reader): C.Vec3 {
  return {
    x: input.f64(),
    y: input.f64(),
    z: input.f64(),
  };
}

export function encodeQuat(out: Writer, value: C.Quat): void {
  out.f64(value.w);
  out.f64(value.x);
  out.f64(value.y);
  out.f64(value.z);
}

export function decodeQuat(input: Reader): C.Quat {
  return {
    w: input.f64(),
    x: input.f64(),
    y: input.f64(),
    z: input.f64(),
  };
}

export function encodeImuSample(out: Writer, value: C.ImuSample): void {
  out.f64(value.timestampNs);
  encodeVec3(out, value.angularVelocity);
  encodeVec3(out, value.acceleration);
  out.bool(value.hasMagnetometer);
  encodeVec3(out, value.magneticField);
  out.bool(value.hasOrientation);
  encodeQuat(out, value.orientation);
}

export function decodeImuSample(input: Reader): C.ImuSample {
  return {
    timestampNs: input.f64(),
    angularVelocity: decodeVec3(input),
    acceleration: decodeVec3(input),
    hasMagnetometer: input.bool(),
    magneticField: decodeVec3(input),
    hasOrientation: input.bool(),
    orientation: decodeQuat(input),
  };
}

export function encodePoseSample(out: Writer, value: C.PoseSample): void {
  out.f64(value.timestampNs);
  encodeQuat(out, value.orientation);
  encodeVec3(out, value.angularVelocity);
  out.f64(value.confidence);
  out.bool(value.visuallyCorrected);
}

export function decodePoseSample(input: Reader): C.PoseSample {
  return {
    timestampNs: input.f64(),
    orientation: decodeQuat(input),
    angularVelocity: decodeVec3(input),
    confidence: input.f64(),
    visuallyCorrected: input.bool(),
  };
}

export function encodeFrameRef(out: Writer, value: C.FrameRef): void {
  out.f64(value.id);
  out.f64(value.buffer);
  out.i32(PixelFormatValues.indexOf(value.format));
  out.f64(value.width);
  out.f64(value.height);
  out.f64(value.stride);
  out.f64(value.timestampNs);
  out.u64(value.contentHash);
}

export function decodeFrameRef(input: Reader): C.FrameRef {
  return {
    id: input.f64() as C.FrameId,
    buffer: input.f64() as C.BufferId,
    format: PixelFormatValues[input.i32()],
    width: input.f64(),
    height: input.f64(),
    stride: input.f64(),
    timestampNs: input.f64(),
    contentHash: input.u64(),
  };
}

export function encodeCapturePlanSpec(out: Writer, value: C.CapturePlanSpec): void {
  out.i32(TessellationStrategyValues.indexOf(value.strategy));
  out.f64(value.horizontalFovDeg);
  out.f64(value.verticalFovDeg);
  out.f64(value.overlapTarget);
  out.f64(value.acceptanceConeDeg);
  out.bool(value.coverPoles);
  out.i32(MotionCapabilityValues.indexOf(value.motion));
}

export function decodeCapturePlanSpec(input: Reader): C.CapturePlanSpec {
  return {
    strategy: TessellationStrategyValues[input.i32()],
    horizontalFovDeg: input.f64(),
    verticalFovDeg: input.f64(),
    overlapTarget: input.f64(),
    acceptanceConeDeg: input.f64(),
    coverPoles: input.bool(),
    motion: MotionCapabilityValues[input.i32()],
  };
}

export function encodeCoverageNode(out: Writer, value: C.CoverageNode): void {
  out.f64(value.id);
  encodeQuat(out, value.targetOrientation);
  out.f64(value.acceptanceConeDeg);
  out.f64(value.ringIndex);
}

export function decodeCoverageNode(input: Reader): C.CoverageNode {
  return {
    id: input.f64() as C.NodeId,
    targetOrientation: decodeQuat(input),
    acceptanceConeDeg: input.f64(),
    ringIndex: input.f64(),
  };
}

export function encodeCapturePlan(out: Writer, value: C.CapturePlan): void {
  out.count(value.nodes.length);
  for (const item of value.nodes) { encodeCoverageNode(out, item); }
  encodeCapturePlanSpec(out, value.spec);
}

export function decodeCapturePlan(input: Reader): C.CapturePlan {
  return {
    nodes: Array.from({ length: input.count() }, () => decodeCoverageNode(input)),
    spec: decodeCapturePlanSpec(input),
  };
}

export function encodeQualityScore(out: Writer, value: C.QualityScore): void {
  out.f64(value.sharpness);
  out.f64(value.motionBlur);
  out.f64(value.exposureAgreement);
  out.f64(value.alignmentResidual);
  out.f64(value.moverPenalty);
  out.f64(value.aggregate);
}

export function decodeQualityScore(input: Reader): C.QualityScore {
  return {
    sharpness: input.f64(),
    motionBlur: input.f64(),
    exposureAgreement: input.f64(),
    alignmentResidual: input.f64(),
    moverPenalty: input.f64(),
    aggregate: input.f64(),
  };
}

export function encodeCandidate(out: Writer, value: C.Candidate): void {
  out.f64(value.id);
  out.f64(value.node);
  encodeFrameRef(out, value.frame);
  encodePoseSample(out, value.pose);
  encodeQualityScore(out, value.quality);
}

export function decodeCandidate(input: Reader): C.Candidate {
  return {
    id: input.f64() as C.CandidateId,
    node: input.f64() as C.NodeId,
    frame: decodeFrameRef(input),
    pose: decodePoseSample(input),
    quality: decodeQualityScore(input),
  };
}

export function encodeBurstSpec(out: Writer, value: C.BurstSpec): void {
  out.f64(value.frameCount);
  out.f64(value.intervalMs);
  out.bool(value.lockExposure);
  out.bool(value.lockWhiteBalance);
  out.bool(value.lockFocus);
}

export function decodeBurstSpec(input: Reader): C.BurstSpec {
  return {
    frameCount: input.f64(),
    intervalMs: input.f64(),
    lockExposure: input.bool(),
    lockWhiteBalance: input.bool(),
    lockFocus: input.bool(),
  };
}

export function encodeCaptureGuidance(out: Writer, value: C.CaptureGuidance): void {
  out.f64(value.targetNode);
  out.f64(value.angularErrorDeg);
  out.f64(value.rollErrorDeg);
  out.f64(value.stability);
  out.i32(GuidanceActionValues.indexOf(value.action));
}

export function decodeCaptureGuidance(input: Reader): C.CaptureGuidance {
  return {
    targetNode: input.f64() as C.NodeId,
    angularErrorDeg: input.f64(),
    rollErrorDeg: input.f64(),
    stability: input.f64(),
    action: GuidanceActionValues[input.i32()],
  };
}

export function encodeCoverageState(out: Writer, value: C.CoverageState): void {
  out.f64(value.nodesTotal);
  out.f64(value.nodesSatisfied);
  out.f64(value.coveredSolidAngleFraction);
  out.count(value.holes.length);
  for (const item of value.holes) { out.f64(item); }
  out.count(value.underOverlapped.length);
  for (const item of value.underOverlapped) { out.f64(item); }
}

export function decodeCoverageState(input: Reader): C.CoverageState {
  return {
    nodesTotal: input.f64(),
    nodesSatisfied: input.f64(),
    coveredSolidAngleFraction: input.f64(),
    holes: Array.from({ length: input.count() }, () => input.f64() as C.NodeId),
    underOverlapped: Array.from({ length: input.count() }, () => input.f64() as C.NodeId),
  };
}

export function encodeBuildSpec(out: Writer, value: C.BuildSpec): void {
  out.i32(QualityTierValues.indexOf(value.tier));
  out.i32(ProjectionValues.indexOf(value.projection));
  out.f64(value.outputWidth);
  out.bool(value.ghostAware);
}

export function decodeBuildSpec(input: Reader): C.BuildSpec {
  return {
    tier: QualityTierValues[input.i32()],
    projection: ProjectionValues[input.i32()],
    outputWidth: input.f64(),
    ghostAware: input.bool(),
  };
}

export function encodeBuildProgress(out: Writer, value: C.BuildProgress): void {
  out.f64(value.id);
  out.i32(BuildStageValues.indexOf(value.stage));
  out.f64(value.fraction);
  out.f64(value.tilesReady);
  out.f64(value.tilesTotal);
  encodeStatus(out, value.failure);
}

export function decodeBuildProgress(input: Reader): C.BuildProgress {
  return {
    id: input.f64() as C.BuildId,
    stage: BuildStageValues[input.i32()],
    fraction: input.f64(),
    tilesReady: input.f64(),
    tilesTotal: input.f64(),
    failure: decodeStatus(input),
  };
}

export function encodeGhostRegion(out: Writer, value: C.GhostRegion): void {
  out.f64(value.node);
  out.f64(value.centerAzimuthDeg);
  out.f64(value.centerElevationDeg);
  out.f64(value.radiusDeg);
  out.f64(value.confidence);
}

export function decodeGhostRegion(input: Reader): C.GhostRegion {
  return {
    node: input.f64() as C.NodeId,
    centerAzimuthDeg: input.f64(),
    centerElevationDeg: input.f64(),
    radiusDeg: input.f64(),
    confidence: input.f64(),
  };
}

export function encodeGhostReport(out: Writer, value: C.GhostReport): void {
  out.count(value.regions.length);
  for (const item of value.regions) { encodeGhostRegion(out, item); }
}

export function decodeGhostReport(input: Reader): C.GhostReport {
  return {
    regions: Array.from({ length: input.count() }, () => decodeGhostRegion(input)),
  };
}

export function encodeEncodeSpec(out: Writer, value: C.EncodeSpec): void {
  out.i32(EncodeFormatValues.indexOf(value.format));
  out.f64(value.quality);
  out.bool(value.attachGPanoXmp);
  out.f64(value.fullPanoWidth);
  out.f64(value.fullPanoHeight);
}

export function decodeEncodeSpec(input: Reader): C.EncodeSpec {
  return {
    format: EncodeFormatValues[input.i32()],
    quality: input.f64(),
    attachGPanoXmp: input.bool(),
    fullPanoWidth: input.f64(),
    fullPanoHeight: input.f64(),
  };
}

export function encodePanoramaRef(out: Writer, value: C.PanoramaRef): void {
  out.f64(value.build);
  out.i32(ProjectionValues.indexOf(value.projection));
  out.f64(value.width);
  out.f64(value.height);
  out.f64(value.tileSize);
  out.count(value.tiles.length);
  for (const item of value.tiles) { encodeFrameRef(out, item); }
  encodeFrameRef(out, value.preview);
}

export function decodePanoramaRef(input: Reader): C.PanoramaRef {
  return {
    build: input.f64() as C.BuildId,
    projection: ProjectionValues[input.i32()],
    width: input.f64(),
    height: input.f64(),
    tileSize: input.f64(),
    tiles: Array.from({ length: input.count() }, () => decodeFrameRef(input)),
    preview: decodeFrameRef(input),
  };
}

export function encodeProjectSummary(out: Writer, value: C.ProjectSummary): void {
  out.f64(value.id);
  out.string(value.title);
  out.f64(value.createdAtMs);
  out.f64(value.nodesTotal);
  out.f64(value.nodesSatisfied);
  out.bool(value.hasBuild);
}

export function decodeProjectSummary(input: Reader): C.ProjectSummary {
  return {
    id: input.f64() as C.ProjectId,
    title: input.string(),
    createdAtMs: input.f64(),
    nodesTotal: input.f64(),
    nodesSatisfied: input.f64(),
    hasBuild: input.bool(),
  };
}

export function encodeExportSpec(out: Writer, value: C.ExportSpec): void {
  encodeEncodeSpec(out, value.encode);
  out.string(value.filename);
  out.bool(value.share);
}

export function decodeExportSpec(input: Reader): C.ExportSpec {
  return {
    encode: decodeEncodeSpec(input),
    filename: input.string(),
    share: input.bool(),
  };
}
