#pragma once

#include <map>
#include <string>

#include "sphanorama/managers/project_manager.h"
#include "sphanorama/resource_access/export_access.h"
#include "sphanorama/resource_access/project_store_access.h"

namespace sphanorama {

// Project lifecycle and export. The only manager that touches IExportAccess, because export is
// the one place bytes leave the device and that should be reachable from exactly one place.
class ProjectManager final : public IProjectManager {
 public:
  ProjectManager(IProjectStoreAccess& store, IExportAccess& exporter);

  Result<std::vector<ProjectSummary>> List() override;
  Result<ProjectId> Create(std::string_view title) override;
  Result<SessionId> Resume(ProjectId project) override;
  Status Delete(ProjectId project) override;
  Status SetSelection(ProjectId project, NodeId node, CandidateId candidate) override;
  Status Export(ProjectId project, BuildId build, const ExportSpec& spec) override;

 private:
  bool Exists(ProjectId project);

  IProjectStoreAccess& store_;
  IExportAccess& exporter_;
  uint64_t next_project_ = 1;
};

}  // namespace sphanorama
