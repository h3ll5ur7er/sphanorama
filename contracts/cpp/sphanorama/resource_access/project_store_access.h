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
  virtual Result<std::string> ReadDocument(ProjectId project, std::string_view key) = 0;
  virtual Status WriteDocument(ProjectId project, std::string_view key, std::string_view value) = 0;
  virtual Status DeleteProject(ProjectId project) = 0;
};

}  // namespace sphanorama
