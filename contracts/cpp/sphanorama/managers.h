// Manager contracts: use-case sequence. The only stateful business components, and the only
// components a client is allowed to call. Managers never call one another.
#pragma once

#include <span>
#include "types.h"

namespace sphanorama {

enum class FrameVerdict : uint8_t { Accepted, RejectedQuality, RejectedPose, BurstComplete };

// V1 — how a capture session is sequenced.
class ICaptureSessionManager {
 public:
  virtual ~ICaptureSessionManager() = default;

  virtual Result<SessionId> Begin(ProjectId, const CapturePlanSpec&) = 0;
  virtual Result<CapturePlan> GetPlan() const = 0;

  // Called at sensor rate from the capture loop. Cheap by contract.
  virtual Result<CaptureGuidance> OnMotion(std::span<const ImuSample>) = 0;

  // Fires a burst at the current cell and folds the results into its candidate set.
  virtual Result<std::vector<Candidate>> CaptureCell(NodeId, const BurstSpec&) = 0;

  // For externally sourced frames (file import, replayed datasets, manual shutter).
  virtual Result<FrameVerdict> OfferFrame(NodeId, const FrameRef&, const PoseSample&) = 0;

  virtual Result<CoverageState> Coverage() const = 0;
  virtual Result<std::vector<Candidate>> Candidates(NodeId) const = 0;

  // Re-arms a cell. Existing candidates are kept unless `replace` is set, so a retake can add
  // to the pool rather than discard evidence.
  virtual Status RequestRetake(NodeId, bool replace) = 0;

  virtual Status End() = 0;
};

// V2 — how a panorama is built, including incremental rebuild.
class IPanoramaBuildManager {
 public:
  virtual ~IPanoramaBuildManager() = default;

  virtual Result<BuildId> Start(SessionId, const BuildSpec&) = 0;
  virtual Result<BuildProgress> Poll(BuildId) = 0;
  virtual Result<PanoramaRef> Result_(BuildId) = 0;
  virtual Result<GhostReport> Ghosts(BuildId) = 0;

  // The mechanism behind retakes AND manual candidate switching: recompute only the
  // transitive closure downstream of the changed cells (docs/04 §4.4).
  virtual Status Invalidate(BuildId, std::span<const NodeId> dirty) = 0;

  virtual Status Cancel(BuildId) = 0;
};

// V3 — project lifecycle and export.
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

class IProjectManager {
 public:
  virtual ~IProjectManager() = default;

  virtual Result<std::vector<ProjectSummary>> List() = 0;
  virtual Result<ProjectId> Create(std::string_view title) = 0;
  virtual Result<SessionId> Resume(ProjectId) = 0;
  virtual Status Delete(ProjectId) = 0;

  // A manual override of automatic burst selection. Marks the node dirty for the next build.
  virtual Status SetSelection(ProjectId, NodeId, CandidateId) = 0;

  virtual Status Export(ProjectId, BuildId, const ExportSpec&) = 0;
};

}  // namespace sphanorama
