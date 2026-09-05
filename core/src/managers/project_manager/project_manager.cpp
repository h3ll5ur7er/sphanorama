#include "managers/project_manager/project_manager.h"

#include <algorithm>

namespace sphanorama {
namespace {
constexpr const char* kComponent = "ProjectManager";
constexpr const char* kTitleKey = "title";
// Written by CaptureSessionManager, read here and nowhere else in this file. A key, not a format:
// what is inside it belongs to the manager that wrote it, and this one never opens it — the
// listing answers "is there something to come back to", which is a fact about the project rather
// than about the session (ADR 0036). Reading across is the shape the store already has: Begin
// reads `title`, which is this manager's, for the same kind of reason.
constexpr const char* kSessionKey = "session";
}  // namespace

ProjectManager::ProjectManager(IProjectStoreAccess& store) : store_(store) {}

bool ProjectManager::Exists(ProjectId project) {
  return store_.ReadDocument(project, kTitleKey).ok();
}

Result<std::vector<ProjectSummary>> ProjectManager::List() {
  SPH_TRY(auto ids, store_.ListProjects());

  std::vector<ProjectSummary> summaries;
  summaries.reserve(ids.size());
  for (const ProjectId id : ids) {
    ProjectSummary summary;
    summary.id = id;
    if (auto title = store_.ReadDocument(id, kTitleKey); title.ok()) summary.title = title.value;
    // Asked of the store on every listing rather than remembered. A session document appears
    // while this manager is alive — the capture manager checkpoints one on the way out of every
    // burst — so anything cached here would report the tab's own capture as unresumable, and
    // that tab is the one holding the phone.
    summary.hasSession = store_.ReadDocument(id, kSessionKey).ok();
    summaries.push_back(std::move(summary));
  }
  return Ok(std::move(summaries));
}

Result<ProjectId> ProjectManager::Create(std::string_view title) {
  if (title.empty()) {
    // An untitled project is indistinguishable from every other one in the listing, which is the
    // single screen where a user has to tell them apart.
    return Err<ProjectId>(StatusCode::InvalidArgument, kComponent, "a project needs a title");
  }
  // Asked of the store, not of a counter. The manager is rebuilt on every page load and the
  // store is not, so a member that restarts at 1 hands out an id that is already taken — and
  // WriteDocument, which creates on demand, then overwrites a real project's title instead of
  // failing. Silent data loss, one reload later.
  SPH_TRY(auto existing, store_.ListProjects());
  for (const ProjectId taken : existing) {
    next_project_ = std::max(next_project_, taken.value + 1);
  }

  const ProjectId id{next_project_++};
  if (auto written = store_.WriteDocument(id, kTitleKey, title); !written.ok()) return written;
  return Ok(id);
}

Status ProjectManager::Delete(ProjectId project) {
  return store_.DeleteProject(project);
}

Status ProjectManager::SetSelection(ProjectId project, NodeId node, CandidateId candidate) {
  if (!Exists(project)) return Fail(StatusCode::NotFound, kComponent, "no such project");
  // Recorded so the next build can treat it exactly like a retake: one dirty node, one partial
  // rebuild (ADR 0004).
  return store_.WriteDocument(project, "selection/" + std::to_string(node.value),
                              std::to_string(candidate.value));
}

Status ProjectManager::Export(ProjectId project, BuildId, const ExportSpec&) {
  if (!Exists(project)) return Fail(StatusCode::NotFound, kComponent, "no such project");
  return Fail(StatusCode::Unsupported, kComponent,
              "nothing to export until the build pipeline lands in Phase 2");
}

}  // namespace sphanorama
