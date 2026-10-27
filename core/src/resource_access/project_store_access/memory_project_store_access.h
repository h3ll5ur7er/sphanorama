#pragma once

#include <map>
#include <string>

#include "sphanorama/resource_access/project_store_access.h"

namespace sphanorama {

// Documents held in memory for the lifetime of the process.
//
// This is a real implementation, not a stand-in: it is what the native bench uses, where a CLI
// run has no browser storage and wants none. The browser gets an IndexedDB-backed port instead,
// and both are held to the same contract suite — which is the point of having one (ADR 0010).
class MemoryProjectStoreAccess final : public IProjectStoreAccess {
 public:
  Result<std::vector<ProjectId>> ListProjects() override;
  Result<ProjectId> IssueProjectId() override;
  Result<std::string> ReadDocument(ProjectId project, std::string_view key) override;
  Status WriteDocument(ProjectId project, std::string_view key, std::string_view value) override;
  Status DeleteProject(ProjectId project) override;

 private:
  std::map<uint64_t, std::map<std::string, std::string, std::less<>>> projects_;
  // The highest id issued or written: deleting a project does not hand its id out again.
  uint64_t highest_ = 0;
};

}  // namespace sphanorama
