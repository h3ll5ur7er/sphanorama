#pragma once

#include <map>
#include <string>

#include "sphanorama/managers/project_manager.h"
#include "sphanorama/resource_access/project_store_access.h"

namespace sphanorama {

// Project lifecycle and export.
//
// Export is the one place bytes leave the device, so when it is implemented this will be the only
// manager holding IExportAccess. It does not hold one yet: Export refuses before it would reach
// for it, and a constructor taking a dependency nothing uses is a claim rather than a need.
class ProjectManager final : public IProjectManager {
 public:
  explicit ProjectManager(IProjectStoreAccess& store);

  Result<std::vector<ProjectSummary>> List() override;
  Result<ProjectId> Create(std::string_view title) override;
  Result<SessionId> Resume(ProjectId project) override;
  Status Delete(ProjectId project) override;
  Status SetSelection(ProjectId project, NodeId node, CandidateId candidate) override;
  Status Export(ProjectId project, BuildId build, const ExportSpec& spec) override;

 private:
  bool Exists(ProjectId project);

  IProjectStoreAccess& store_;
  uint64_t next_project_ = 1;
};

}  // namespace sphanorama
