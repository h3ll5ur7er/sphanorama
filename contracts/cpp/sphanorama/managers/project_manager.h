#pragma once
#include <string_view>
#include "sphanorama/types.h"

namespace sphanorama {

// V3 — project lifecycle and export. The only manager that touches IExportAccess.
// @boundary @facade
class IProjectManager {
 public:
  virtual ~IProjectManager() = default;

  // No Resume here, deliberately. Picking a capture back up means handing a live session to
  // whatever owns session state, and that is `ICaptureSessionManager` — this manager could only
  // ever have returned a SessionId it had no way to make (ADR 0029).
  virtual Result<std::vector<ProjectSummary>> List() = 0;
  virtual Result<ProjectId> Create(std::string_view title) = 0;
  virtual Status Delete(ProjectId project) = 0;

  // A manual override of automatic burst selection. Marks the node dirty for the next build, so
  // it takes exactly the same path as a retake.
  virtual Status SetSelection(ProjectId project, NodeId node, CandidateId candidate) = 0;

  virtual Status Export(ProjectId project, BuildId build, const ExportSpec& spec) = 0;
};

}  // namespace sphanorama
