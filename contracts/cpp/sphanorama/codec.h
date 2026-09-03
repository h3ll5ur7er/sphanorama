// GENERATED FILE — DO NOT EDIT. Produced by tools/contract_gen.py.
//
// Per-type encoders over the hand-written primitives in sphanorama/wire.h. Both this and the
// TypeScript codec come from one parse of the contract headers, so the two sides cannot disagree
// about field order or widths — the failure that would decode into plausible nonsense rather
// than failing (ADR 0013).
//
// Only types reachable from a @boundary method are emitted: nothing else crosses.
#pragma once

#include "sphanorama/types.h"
#include "sphanorama/wire.h"

namespace sphanorama::codec {

using wire::Reader;
using wire::Writer;

void Encode(Writer& out, const Status& value);
bool Decode(Reader& in, Status& value);
void Encode(Writer& out, const Vec3& value);
bool Decode(Reader& in, Vec3& value);
void Encode(Writer& out, const Quat& value);
bool Decode(Reader& in, Quat& value);
void Encode(Writer& out, const ImuSample& value);
bool Decode(Reader& in, ImuSample& value);
void Encode(Writer& out, const PoseSample& value);
bool Decode(Reader& in, PoseSample& value);
void Encode(Writer& out, const FrameRef& value);
bool Decode(Reader& in, FrameRef& value);
void Encode(Writer& out, const CapturePlanSpec& value);
bool Decode(Reader& in, CapturePlanSpec& value);
void Encode(Writer& out, const CoverageNode& value);
bool Decode(Reader& in, CoverageNode& value);
void Encode(Writer& out, const CapturePlan& value);
bool Decode(Reader& in, CapturePlan& value);
void Encode(Writer& out, const QualityScore& value);
bool Decode(Reader& in, QualityScore& value);
void Encode(Writer& out, const Candidate& value);
bool Decode(Reader& in, Candidate& value);
void Encode(Writer& out, const BurstSpec& value);
bool Decode(Reader& in, BurstSpec& value);
void Encode(Writer& out, const CaptureGuidance& value);
bool Decode(Reader& in, CaptureGuidance& value);
void Encode(Writer& out, const CoverageState& value);
bool Decode(Reader& in, CoverageState& value);
void Encode(Writer& out, const BuildSpec& value);
bool Decode(Reader& in, BuildSpec& value);
void Encode(Writer& out, const BuildProgress& value);
bool Decode(Reader& in, BuildProgress& value);
void Encode(Writer& out, const GhostRegion& value);
bool Decode(Reader& in, GhostRegion& value);
void Encode(Writer& out, const GhostReport& value);
bool Decode(Reader& in, GhostReport& value);
void Encode(Writer& out, const EncodeSpec& value);
bool Decode(Reader& in, EncodeSpec& value);
void Encode(Writer& out, const PanoramaRef& value);
bool Decode(Reader& in, PanoramaRef& value);
void Encode(Writer& out, const ProjectSummary& value);
bool Decode(Reader& in, ProjectSummary& value);
void Encode(Writer& out, const ExportSpec& value);
bool Decode(Reader& in, ExportSpec& value);

inline void Encode(Writer& out, const Status& value) {
  out.PutI32(static_cast<int32_t>(value.code));
  out.PutString(value.component);
  out.PutString(value.detail);
}

inline bool Decode(Reader& in, Status& value) {
  value.code = static_cast<StatusCode>(in.GetI32());
  value.component = in.GetString();
  value.detail = in.GetString();
  return in.ok();
}

inline void Encode(Writer& out, const Vec3& value) {
  out.PutF64(static_cast<double>(value.x));
  out.PutF64(static_cast<double>(value.y));
  out.PutF64(static_cast<double>(value.z));
}

inline bool Decode(Reader& in, Vec3& value) {
  value.x = static_cast<decltype(value.x)>(in.GetF64());
  value.y = static_cast<decltype(value.y)>(in.GetF64());
  value.z = static_cast<decltype(value.z)>(in.GetF64());
  return in.ok();
}

inline void Encode(Writer& out, const Quat& value) {
  out.PutF64(static_cast<double>(value.w));
  out.PutF64(static_cast<double>(value.x));
  out.PutF64(static_cast<double>(value.y));
  out.PutF64(static_cast<double>(value.z));
}

inline bool Decode(Reader& in, Quat& value) {
  value.w = static_cast<decltype(value.w)>(in.GetF64());
  value.x = static_cast<decltype(value.x)>(in.GetF64());
  value.y = static_cast<decltype(value.y)>(in.GetF64());
  value.z = static_cast<decltype(value.z)>(in.GetF64());
  return in.ok();
}

inline void Encode(Writer& out, const ImuSample& value) {
  out.PutF64(static_cast<double>(value.timestampNs));
  Encode(out, value.angularVelocity);
  Encode(out, value.acceleration);
  out.PutBool(value.hasMagnetometer);
  Encode(out, value.magneticField);
  out.PutBool(value.hasOrientation);
  Encode(out, value.orientation);
}

inline bool Decode(Reader& in, ImuSample& value) {
  value.timestampNs = static_cast<decltype(value.timestampNs)>(in.GetF64());
  if (!Decode(in, value.angularVelocity)) return false;
  if (!Decode(in, value.acceleration)) return false;
  value.hasMagnetometer = in.GetBool();
  if (!Decode(in, value.magneticField)) return false;
  value.hasOrientation = in.GetBool();
  if (!Decode(in, value.orientation)) return false;
  return in.ok();
}

inline void Encode(Writer& out, const PoseSample& value) {
  out.PutF64(static_cast<double>(value.timestampNs));
  Encode(out, value.orientation);
  Encode(out, value.angularVelocity);
  out.PutF64(static_cast<double>(value.confidence));
  out.PutBool(value.visuallyCorrected);
}

inline bool Decode(Reader& in, PoseSample& value) {
  value.timestampNs = static_cast<decltype(value.timestampNs)>(in.GetF64());
  if (!Decode(in, value.orientation)) return false;
  if (!Decode(in, value.angularVelocity)) return false;
  value.confidence = static_cast<decltype(value.confidence)>(in.GetF64());
  value.visuallyCorrected = in.GetBool();
  return in.ok();
}

inline void Encode(Writer& out, const FrameRef& value) {
  out.PutF64(static_cast<double>(value.id.value));
  out.PutF64(static_cast<double>(value.buffer.value));
  out.PutI32(static_cast<int32_t>(value.format));
  out.PutF64(static_cast<double>(value.width));
  out.PutF64(static_cast<double>(value.height));
  out.PutF64(static_cast<double>(value.stride));
  out.PutF64(static_cast<double>(value.timestampNs));
  out.PutU64(value.contentHash);
}

inline bool Decode(Reader& in, FrameRef& value) {
  value.id.value = in.GetId();
  value.buffer.value = in.GetId();
  value.format = static_cast<PixelFormat>(in.GetI32());
  value.width = static_cast<decltype(value.width)>(in.GetF64());
  value.height = static_cast<decltype(value.height)>(in.GetF64());
  value.stride = static_cast<decltype(value.stride)>(in.GetF64());
  value.timestampNs = static_cast<decltype(value.timestampNs)>(in.GetF64());
  value.contentHash = in.GetU64();
  return in.ok();
}

inline void Encode(Writer& out, const CapturePlanSpec& value) {
  out.PutI32(static_cast<int32_t>(value.strategy));
  out.PutF64(static_cast<double>(value.horizontalFovDeg));
  out.PutF64(static_cast<double>(value.verticalFovDeg));
  out.PutF64(static_cast<double>(value.overlapTarget));
  out.PutF64(static_cast<double>(value.acceptanceConeDeg));
  out.PutBool(value.coverPoles);
  out.PutI32(static_cast<int32_t>(value.motion));
}

inline bool Decode(Reader& in, CapturePlanSpec& value) {
  value.strategy = static_cast<TessellationStrategy>(in.GetI32());
  value.horizontalFovDeg = static_cast<decltype(value.horizontalFovDeg)>(in.GetF64());
  value.verticalFovDeg = static_cast<decltype(value.verticalFovDeg)>(in.GetF64());
  value.overlapTarget = static_cast<decltype(value.overlapTarget)>(in.GetF64());
  value.acceptanceConeDeg = static_cast<decltype(value.acceptanceConeDeg)>(in.GetF64());
  value.coverPoles = in.GetBool();
  value.motion = static_cast<MotionCapability>(in.GetI32());
  return in.ok();
}

inline void Encode(Writer& out, const CoverageNode& value) {
  out.PutF64(static_cast<double>(value.id.value));
  Encode(out, value.targetOrientation);
  out.PutF64(static_cast<double>(value.acceptanceConeDeg));
  out.PutF64(static_cast<double>(value.ringIndex));
}

inline bool Decode(Reader& in, CoverageNode& value) {
  value.id.value = in.GetId();
  if (!Decode(in, value.targetOrientation)) return false;
  value.acceptanceConeDeg = static_cast<decltype(value.acceptanceConeDeg)>(in.GetF64());
  value.ringIndex = static_cast<decltype(value.ringIndex)>(in.GetF64());
  return in.ok();
}

inline void Encode(Writer& out, const CapturePlan& value) {
  out.PutCount(value.nodes.size());
  for (const auto& item : value.nodes) { Encode(out, item); }
  Encode(out, value.spec);
}

inline bool Decode(Reader& in, CapturePlan& value) {
  { const size_t count = in.GetCount(56);
    if (!in.ok()) return false;
    value.nodes.clear();
    value.nodes.resize(count);
    for (auto& item : value.nodes) { if (!Decode(in, item)) return false; } }
  if (!Decode(in, value.spec)) return false;
  return in.ok();
}

inline void Encode(Writer& out, const QualityScore& value) {
  out.PutF64(static_cast<double>(value.sharpness));
  out.PutF64(static_cast<double>(value.motionBlur));
  out.PutF64(static_cast<double>(value.exposureAgreement));
  out.PutF64(static_cast<double>(value.alignmentResidual));
  out.PutF64(static_cast<double>(value.moverPenalty));
  out.PutF64(static_cast<double>(value.aggregate));
}

inline bool Decode(Reader& in, QualityScore& value) {
  value.sharpness = static_cast<decltype(value.sharpness)>(in.GetF64());
  value.motionBlur = static_cast<decltype(value.motionBlur)>(in.GetF64());
  value.exposureAgreement = static_cast<decltype(value.exposureAgreement)>(in.GetF64());
  value.alignmentResidual = static_cast<decltype(value.alignmentResidual)>(in.GetF64());
  value.moverPenalty = static_cast<decltype(value.moverPenalty)>(in.GetF64());
  value.aggregate = static_cast<decltype(value.aggregate)>(in.GetF64());
  return in.ok();
}

inline void Encode(Writer& out, const Candidate& value) {
  out.PutF64(static_cast<double>(value.id.value));
  out.PutF64(static_cast<double>(value.node.value));
  Encode(out, value.frame);
  Encode(out, value.pose);
  Encode(out, value.quality);
}

inline bool Decode(Reader& in, Candidate& value) {
  value.id.value = in.GetId();
  value.node.value = in.GetId();
  if (!Decode(in, value.frame)) return false;
  if (!Decode(in, value.pose)) return false;
  if (!Decode(in, value.quality)) return false;
  return in.ok();
}

inline void Encode(Writer& out, const BurstSpec& value) {
  out.PutF64(static_cast<double>(value.frameCount));
  out.PutF64(static_cast<double>(value.intervalMs));
  out.PutBool(value.lockExposure);
  out.PutBool(value.lockWhiteBalance);
  out.PutBool(value.lockFocus);
}

inline bool Decode(Reader& in, BurstSpec& value) {
  value.frameCount = static_cast<decltype(value.frameCount)>(in.GetF64());
  value.intervalMs = static_cast<decltype(value.intervalMs)>(in.GetF64());
  value.lockExposure = in.GetBool();
  value.lockWhiteBalance = in.GetBool();
  value.lockFocus = in.GetBool();
  return in.ok();
}

inline void Encode(Writer& out, const CaptureGuidance& value) {
  out.PutF64(static_cast<double>(value.targetNode.value));
  out.PutF64(static_cast<double>(value.angularErrorDeg));
  out.PutF64(static_cast<double>(value.rollErrorDeg));
  out.PutF64(static_cast<double>(value.stability));
  out.PutI32(static_cast<int32_t>(value.action));
}

inline bool Decode(Reader& in, CaptureGuidance& value) {
  value.targetNode.value = in.GetId();
  value.angularErrorDeg = static_cast<decltype(value.angularErrorDeg)>(in.GetF64());
  value.rollErrorDeg = static_cast<decltype(value.rollErrorDeg)>(in.GetF64());
  value.stability = static_cast<decltype(value.stability)>(in.GetF64());
  value.action = static_cast<GuidanceAction>(in.GetI32());
  return in.ok();
}

inline void Encode(Writer& out, const CoverageState& value) {
  out.PutF64(static_cast<double>(value.nodesTotal));
  out.PutF64(static_cast<double>(value.nodesSatisfied));
  out.PutF64(static_cast<double>(value.coveredSolidAngleFraction));
  out.PutCount(value.holes.size());
  for (const auto& item : value.holes) { out.PutF64(static_cast<double>(item.value)); }
  out.PutCount(value.underOverlapped.size());
  for (const auto& item : value.underOverlapped) { out.PutF64(static_cast<double>(item.value)); }
}

inline bool Decode(Reader& in, CoverageState& value) {
  value.nodesTotal = static_cast<decltype(value.nodesTotal)>(in.GetF64());
  value.nodesSatisfied = static_cast<decltype(value.nodesSatisfied)>(in.GetF64());
  value.coveredSolidAngleFraction = static_cast<decltype(value.coveredSolidAngleFraction)>(in.GetF64());
  { const size_t count = in.GetCount(8);
    if (!in.ok()) return false;
    value.holes.clear();
    value.holes.resize(count);
    for (auto& item : value.holes) { item.value = in.GetId(); } }
  { const size_t count = in.GetCount(8);
    if (!in.ok()) return false;
    value.underOverlapped.clear();
    value.underOverlapped.resize(count);
    for (auto& item : value.underOverlapped) { item.value = in.GetId(); } }
  return in.ok();
}

inline void Encode(Writer& out, const BuildSpec& value) {
  out.PutI32(static_cast<int32_t>(value.tier));
  out.PutI32(static_cast<int32_t>(value.projection));
  out.PutF64(static_cast<double>(value.outputWidth));
  out.PutBool(value.ghostAware);
}

inline bool Decode(Reader& in, BuildSpec& value) {
  value.tier = static_cast<QualityTier>(in.GetI32());
  value.projection = static_cast<Projection>(in.GetI32());
  value.outputWidth = static_cast<decltype(value.outputWidth)>(in.GetF64());
  value.ghostAware = in.GetBool();
  return in.ok();
}

inline void Encode(Writer& out, const BuildProgress& value) {
  out.PutF64(static_cast<double>(value.id.value));
  out.PutI32(static_cast<int32_t>(value.stage));
  out.PutF64(static_cast<double>(value.fraction));
  out.PutF64(static_cast<double>(value.tilesReady));
  out.PutF64(static_cast<double>(value.tilesTotal));
  Encode(out, value.failure);
}

inline bool Decode(Reader& in, BuildProgress& value) {
  value.id.value = in.GetId();
  value.stage = static_cast<BuildStage>(in.GetI32());
  value.fraction = static_cast<decltype(value.fraction)>(in.GetF64());
  value.tilesReady = static_cast<decltype(value.tilesReady)>(in.GetF64());
  value.tilesTotal = static_cast<decltype(value.tilesTotal)>(in.GetF64());
  if (!Decode(in, value.failure)) return false;
  return in.ok();
}

inline void Encode(Writer& out, const GhostRegion& value) {
  out.PutF64(static_cast<double>(value.node.value));
  out.PutF64(static_cast<double>(value.centerAzimuthDeg));
  out.PutF64(static_cast<double>(value.centerElevationDeg));
  out.PutF64(static_cast<double>(value.radiusDeg));
  out.PutF64(static_cast<double>(value.confidence));
}

inline bool Decode(Reader& in, GhostRegion& value) {
  value.node.value = in.GetId();
  value.centerAzimuthDeg = static_cast<decltype(value.centerAzimuthDeg)>(in.GetF64());
  value.centerElevationDeg = static_cast<decltype(value.centerElevationDeg)>(in.GetF64());
  value.radiusDeg = static_cast<decltype(value.radiusDeg)>(in.GetF64());
  value.confidence = static_cast<decltype(value.confidence)>(in.GetF64());
  return in.ok();
}

inline void Encode(Writer& out, const GhostReport& value) {
  out.PutCount(value.regions.size());
  for (const auto& item : value.regions) { Encode(out, item); }
}

inline bool Decode(Reader& in, GhostReport& value) {
  { const size_t count = in.GetCount(40);
    if (!in.ok()) return false;
    value.regions.clear();
    value.regions.resize(count);
    for (auto& item : value.regions) { if (!Decode(in, item)) return false; } }
  return in.ok();
}

inline void Encode(Writer& out, const EncodeSpec& value) {
  out.PutI32(static_cast<int32_t>(value.format));
  out.PutF64(static_cast<double>(value.quality));
  out.PutBool(value.attachGPanoXmp);
  out.PutF64(static_cast<double>(value.fullPanoWidth));
  out.PutF64(static_cast<double>(value.fullPanoHeight));
}

inline bool Decode(Reader& in, EncodeSpec& value) {
  value.format = static_cast<EncodeFormat>(in.GetI32());
  value.quality = static_cast<decltype(value.quality)>(in.GetF64());
  value.attachGPanoXmp = in.GetBool();
  value.fullPanoWidth = static_cast<decltype(value.fullPanoWidth)>(in.GetF64());
  value.fullPanoHeight = static_cast<decltype(value.fullPanoHeight)>(in.GetF64());
  return in.ok();
}

inline void Encode(Writer& out, const PanoramaRef& value) {
  out.PutF64(static_cast<double>(value.build.value));
  out.PutI32(static_cast<int32_t>(value.projection));
  out.PutF64(static_cast<double>(value.width));
  out.PutF64(static_cast<double>(value.height));
  out.PutF64(static_cast<double>(value.tileSize));
  out.PutCount(value.tiles.size());
  for (const auto& item : value.tiles) { Encode(out, item); }
  Encode(out, value.preview);
}

inline bool Decode(Reader& in, PanoramaRef& value) {
  value.build.value = in.GetId();
  value.projection = static_cast<Projection>(in.GetI32());
  value.width = static_cast<decltype(value.width)>(in.GetF64());
  value.height = static_cast<decltype(value.height)>(in.GetF64());
  value.tileSize = static_cast<decltype(value.tileSize)>(in.GetF64());
  { const size_t count = in.GetCount(60);
    if (!in.ok()) return false;
    value.tiles.clear();
    value.tiles.resize(count);
    for (auto& item : value.tiles) { if (!Decode(in, item)) return false; } }
  if (!Decode(in, value.preview)) return false;
  return in.ok();
}

inline void Encode(Writer& out, const ProjectSummary& value) {
  out.PutF64(static_cast<double>(value.id.value));
  out.PutString(value.title);
  out.PutF64(static_cast<double>(value.createdAtMs));
  out.PutF64(static_cast<double>(value.nodesTotal));
  out.PutF64(static_cast<double>(value.nodesSatisfied));
  out.PutBool(value.hasBuild);
}

inline bool Decode(Reader& in, ProjectSummary& value) {
  value.id.value = in.GetId();
  value.title = in.GetString();
  value.createdAtMs = static_cast<decltype(value.createdAtMs)>(in.GetF64());
  value.nodesTotal = static_cast<decltype(value.nodesTotal)>(in.GetF64());
  value.nodesSatisfied = static_cast<decltype(value.nodesSatisfied)>(in.GetF64());
  value.hasBuild = in.GetBool();
  return in.ok();
}

inline void Encode(Writer& out, const ExportSpec& value) {
  Encode(out, value.encode);
  out.PutString(value.filename);
  out.PutBool(value.share);
}

inline bool Decode(Reader& in, ExportSpec& value) {
  if (!Decode(in, value.encode)) return false;
  value.filename = in.GetString();
  value.share = in.GetBool();
  return in.ok();
}

}  // namespace sphanorama::codec
