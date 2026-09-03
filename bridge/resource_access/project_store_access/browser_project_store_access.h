#pragma once

#include "sphanorama/resource_access/project_store_access.h"

namespace sphanorama::bridge {

// IProjectStoreAccess backed by the page's document host.
//
// The contract is synchronous and IndexedDB is not, so the host keeps every document resident in
// memory and persists asynchronously behind it (ADR 0014). Reads are memory reads; a write is
// visible immediately and durable shortly after.
//
// Making the contract async instead would push V12's volatility upward into every manager that
// touches storage, which is the opposite of what the resource-access layer is for.
class BrowserProjectStoreAccess final : public IProjectStoreAccess {
 public:
  Result<std::vector<ProjectId>> ListProjects() override;
  Result<std::string> ReadDocument(ProjectId project, std::string_view key) override;
  Status WriteDocument(ProjectId project, std::string_view key, std::string_view value) override;
  Status DeleteProject(ProjectId project) override;
};

}  // namespace sphanorama::bridge
