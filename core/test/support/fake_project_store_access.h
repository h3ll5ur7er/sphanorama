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
  Result<ProjectId> IssueProjectId() override;
  Result<std::string> ReadDocument(ProjectId project, std::string_view key) override;
  Status WriteDocument(ProjectId project, std::string_view key, std::string_view value) override;
  Status DeleteProject(ProjectId project) override;

  int WriteCount() const { return write_count_; }
  // Reads of this key fail with `code`: no store here fails a read with anything but `NotFound`
  // otherwise, and a caller must not read any other failure as absence. Chosen by the test, so a
  // caller that answers a fixed code of its own instead of the store's can be told apart.
  void RefuseReadsOf(std::string key, StatusCode code = StatusCode::StorageQuotaExceeded) {
    unreadable_ = std::move(key);
    unreadable_code_ = code;
  }

 private:
  std::map<uint64_t, std::map<std::string, std::string, std::less<>>> projects_;
  // The highest id issued or written: deleting a project does not hand its id out again.
  uint64_t highest_ = 0;
  int write_count_ = 0;
  std::string unreadable_;
  StatusCode unreadable_code_ = StatusCode::StorageQuotaExceeded;
};

struct FakeProjectStoreAccessFactory {
  static std::unique_ptr<IProjectStoreAccess> Create() {
    return std::make_unique<FakeProjectStoreAccess>();
  }
};

}  // namespace sphanorama
