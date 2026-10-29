#pragma once
#include <string_view>
#include <vector>
#include "sphanorama/types.h"

namespace sphanorama {

// V12 — where project metadata is persisted. Documents only, never pixels: the split is what
// makes "resume after the browser killed the tab" a metadata read plus lazy pixel faulting.
// @boundary
class IProjectStoreAccess {
 public:
  virtual ~IProjectStoreAccess() = default;

  virtual Result<std::vector<ProjectId>> ListProjects() = 0;

  // An identity for a new project, above every id this store holds and every one it has issued
  // since it was opened, removed or not. Issuing creates nothing; the first document written does.
  // One page's ids ascend in the order it issued them, which is how a page tells the newest
  // capture; across pages, only as nearly as ADR 0081 says.
  // `FailedPrecondition` when no id is left that a double holds exactly, since every id crosses to
  // the page as one.
  //
  // The store's to answer rather than the manager's, because the store is the only thing every
  // page shares. A page's listing is a copy taken when it loaded, so one past the highest id in it
  // may be an id another tab has written since, and a write creates on demand. Where another page
  // writes the same storage, an id is one that page all but certainly does not issue too; ADR 0081
  // says how near to certain.
  virtual Result<ProjectId> IssueProjectId() = 0;
  virtual Result<std::string> ReadDocument(ProjectId project, std::string_view key) = 0;
  virtual Status WriteDocument(ProjectId project, std::string_view key, std::string_view value) = 0;
  virtual Status DeleteProject(ProjectId project) = 0;
};

}  // namespace sphanorama
