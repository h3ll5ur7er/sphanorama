#pragma once
#include <string_view>
#include "sphanorama/types.h"

namespace sphanorama {

// V3 — project lifecycle and export. The only manager that touches IExportAccess.
class IProjectManager {
 public:
  virtual ~IProjectManager() = default;

  virtual Result<std::vector<ProjectSummary>> List() = 0;
  virtual Result<ProjectId> Create(std::string_view title) = 0;
  virtual Result<SessionId> Resume(ProjectId) = 0;
  virtual Status Delete(ProjectId) = 0;

  // A manual override of automatic burst selection. Marks the node dirty for the next build, so
  // it takes exactly the same path as a retake.
  virtual Status SetSelection(ProjectId, NodeId, CandidateId) = 0;

  virtual Status Export(ProjectId, BuildId, const ExportSpec&) = 0;
};

}  // namespace sphanorama
