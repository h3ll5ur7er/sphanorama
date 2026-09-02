// Engine contracts: business activities.
//
// Engines are stateless with respect to a session — every call takes its inputs explicitly.
// They never call managers, never call each other, and touch only IComputeDeviceAccess and
// IFrameStoreAccess (injected at construction; see docs/03 §3.3 rule 5).
#pragma once

#include <span>
#include "types.h"

namespace sphanorama {

// V4 — how the sphere is tessellated and coverage is judged.
class ICoveragePlannerEngine {
 public:
  virtual ~ICoveragePlannerEngine() = default;
  virtual Result<CapturePlan> Plan(const CapturePlanSpec&, const Intrinsics& lens) = 0;
  // Which cell is this orientation aiming at, and how far off is it?
  virtual Result<CaptureGuidance> Locate(const Quat& current, const CapturePlan&) = 0;
  virtual Result<CoverageState> Evaluate(const CapturePlan&, std::span<const Candidate>) = 0;
  // Which cells would most improve coverage if re-shot? Drives retake suggestions.
  virtual Result<std::vector<NodeId>> SuggestRetakes(const CapturePlan&,
                                                     const CoverageState&,
                                                     const GhostReport&) = 0;
};

// V5 — how orientation is estimated. Absorbs sensor absence, drift and fusion choice.
enum class PoseMode : uint8_t { Fused, GyroOnly, VisionOnly };

class IPoseEngine {
 public:
  virtual ~IPoseEngine() = default;
  virtual Status Reset(PoseMode, MotionCapability) = 0;
  virtual Result<PoseSample> Integrate(std::span<const ImuSample>) = 0;
  // Optional visual refinement against a reference frame; also the whole of VisionOnly mode.
  virtual Result<PoseSample> Correct(const FrameRef& current,
                                     const FrameRef& reference,
                                     const PoseSample& prior) = 0;
  virtual Result<double> Stability(std::span<const ImuSample>) = 0;   // [0,1]
};

// V6 — what makes a frame the best of its burst. The most-tuned component in the system.
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

class IFrameQualityEngine {
 public:
  virtual ~IFrameQualityEngine() = default;
  virtual Result<QualityScore> Score(const FrameRef&, const PoseSample&, const NodeContext&) = 0;
  // Ranked best-first. The manager decides what to do with the ranking.
  virtual Result<std::vector<CandidateId>> Rank(std::span<const Candidate>,
                                                const SelectionPolicy&) = 0;
};

// V7 — how frames are aligned.
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

class IRegistrationEngine {
 public:
  virtual ~IRegistrationEngine() = default;
  virtual Result<FeatureSet> ExtractFeatures(const FrameRef&) = 0;
  // The sensor pose enters here as a prior that seeds and bounds the search — never as truth.
  virtual Result<PairwiseResult> EstimatePairwise(const FeatureSet& a, const FeatureSet& b,
                                                  const Quat& prior) = 0;
  virtual Result<GlobalSolution> Refine(std::span<const PairwiseResult>,
                                        std::span<const PoseSample> priors,
                                        const Intrinsics& initial) = 0;
};

// V8 — how pixels become one image.
struct GainMap { std::vector<double> perFrameGain; std::vector<FrameId> frames; };
struct SeamMap { BufferId labelBuffer; int32_t width = 0, height = 0; };

class ICompositionEngine {
 public:
  virtual ~ICompositionEngine() = default;
  virtual Result<GainMap> CompensateExposure(const GlobalSolution&,
                                             std::span<const FrameRef>) = 0;
  // Disagreement between a cell's own candidates localises movers directly (docs/04 §4.5).
  virtual Result<GhostReport> DetectGhosts(const GlobalSolution&,
                                           std::span<const Candidate> allCandidates) = 0;
  virtual Result<SeamMap> FindSeams(const GlobalSolution&, const GainMap&,
                                    const GhostReport&, const BuildSpec&) = 0;
  // Tiled so a retake re-blends a handful of tiles rather than the whole sphere.
  virtual Result<FrameRef> BlendTile(const GlobalSolution&, const GainMap&, const SeamMap&,
                                     const BuildSpec&, int32_t tileX, int32_t tileY) = 0;
  virtual Result<FrameRef> RenderPreview(const GlobalSolution&, const GainMap&,
                                         int32_t maxWidth) = 0;
};

}  // namespace sphanorama
