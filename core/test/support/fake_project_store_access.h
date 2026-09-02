#pragma once

#include <map>
#include <memory>
#include <string>

#include "sphanorama/resource_access/project_store_access.h"

namespace sphanorama {

// An in-memory document store. Documents only, never pixels — the same split the real IndexedDB
// store keeps, so that a "resume the session" test exercises metadata reload rather than a
// wholesale restore.
class FakeProjectStoreAccess final : public IProjectStoreAccess {
 public:
  Result<std::vector<ProjectId>> ListProjects() override;
  Result<std::string> ReadDocument(ProjectId project, std::string_view key) override;
  Status WriteDocument(ProjectId project, std::string_view key, std::string_view value) override;
  Status DeleteProject(ProjectId project) override;

  int WriteCount() const { return write_count_; }

 private:
  std::map<uint64_t, std::map<std::string, std::string, std::less<>>> projects_;
  int write_count_ = 0;
};

struct FakeProjectStoreAccessFactory {
  static std::unique_ptr<IProjectStoreAccess> Create() {
    return std::make_unique<FakeProjectStoreAccess>();
  }
};

}  // namespace sphanorama
