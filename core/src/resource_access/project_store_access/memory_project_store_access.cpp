#include "resource_access/project_store_access/memory_project_store_access.h"

#include <algorithm>

namespace sphanorama {
namespace {
constexpr const char* kComponent = "MemoryProjectStoreAccess";
}

Result<std::vector<ProjectId>> MemoryProjectStoreAccess::ListProjects() {
  std::vector<ProjectId> ids;
  ids.reserve(projects_.size());
  for (const auto& [id, documents] : projects_) ids.push_back(ProjectId{id});
  return Ok(std::move(ids));
}

Result<ProjectId> MemoryProjectStoreAccess::IssueProjectId() {
  // One process holds this store, so a counter is never stale. Past what a double holds exactly,
  // the page would read two projects as one.
  if (highest_ >= (uint64_t{1} << 53) - 1) {
    return Err<ProjectId>(StatusCode::FailedPrecondition, kComponent,
                          "no project id left to issue");
  }
  return Ok(ProjectId{++highest_});
}

Result<std::string> MemoryProjectStoreAccess::ReadDocument(ProjectId project,
                                                           std::string_view key) {
  const auto project_it = projects_.find(project.value);
  if (project_it == projects_.end()) {
    return Err<std::string>(StatusCode::NotFound, kComponent, "no such project");
  }
  const auto doc_it = project_it->second.find(key);
  if (doc_it == project_it->second.end()) {
    // Never an empty string: that is indistinguishable from a document written empty, and a
    // resume would silently start from a blank plan.
    return Err<std::string>(StatusCode::NotFound, kComponent, "no such document");
  }
  return Ok(doc_it->second);
}

Status MemoryProjectStoreAccess::WriteDocument(ProjectId project, std::string_view key,
                                               std::string_view value) {
  if (!project.valid()) return Fail(StatusCode::InvalidArgument, kComponent, "project id is unset");
  if (key.empty()) return Fail(StatusCode::InvalidArgument, kComponent, "document key is empty");
  projects_[project.value][std::string(key)] = std::string(value);
  highest_ = std::max(highest_, project.value);
  return Status::Ok();
}

Status MemoryProjectStoreAccess::DeleteProject(ProjectId project) {
  if (projects_.erase(project.value) == 0) {
    return Fail(StatusCode::NotFound, kComponent, "no such project");
  }
  return Status::Ok();
}

}  // namespace sphanorama
