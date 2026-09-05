#include "managers/project_manager/project_manager.h"

#include <algorithm>
#include <charconv>
#include <string>
#include <system_error>

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

// One document per cell, so setting a pick does not read and rewrite the others. The project is in
// the address rather than the key, which is what keeps two spheres that both chose something for
// cell 3 from sharing it.
std::string SelectionKey(NodeId node) {
  return "selection/" + std::to_string(node.value);
}
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
  // Before the project, because this is about the arguments rather than the store. Zero is what
  // `GetSelection` answers for "nobody has chosen here", so a document holding zero would be a
  // contradiction the read path has to call corrupt — a write that creates state its own reader
  // cannot represent. `Id::valid()` is `value != 0` and every counter in these contracts starts
  // at 1, so an unset id is a caller mistake and not a choice anybody made.
  if (!node.valid() || !candidate.valid()) {
    return Fail(StatusCode::InvalidArgument, kComponent,
                "a selection needs a real cell and a real candidate");
  }
  if (!Exists(project)) return Fail(StatusCode::NotFound, kComponent, "no such project");
  // Recorded so the next build can treat it exactly like a retake: one dirty node, one partial
  // rebuild (ADR 0004).
  return store_.WriteDocument(project, SelectionKey(node), std::to_string(candidate.value));
}

Result<CandidateId> ProjectManager::GetSelection(ProjectId project, NodeId node) {
  // The mirror of the rule in `SetSelection`, and it protects the sentinel rather than the store.
  // Nothing can write `selection/0`, so answering "nobody has chosen here" for it would make a
  // caller passing an uninitialised id look exactly like a cell that genuinely has no override.
  // Zero only means something as an answer because every other answer is a real one.
  if (!node.valid()) {
    return Err<CandidateId>(StatusCode::InvalidArgument, kComponent, "that is not a cell");
  }
  if (!Exists(project)) {
    return Err<CandidateId>(StatusCode::NotFound, kComponent, "no such project");
  }
  // An absent document is the ordinary case — most cells are never overridden — so it answers
  // zero rather than failing. The contract says why that is not `NotFound`.
  //
  // `NotFound` and nothing else, because absence is the sentinel and a failure is not absence.
  // Every store today refuses a missing document exactly this way and has no other way to fail,
  // but the contract allows one — and folding a storage error into "nobody has chosen here" would
  // show the ranking's pick for a cell whose override could not be read, which is the screen
  // quietly disagreeing with the build, without a word anywhere.
  auto document = store_.ReadDocument(project, SelectionKey(node));
  if (!document.ok()) {
    if (document.status.code == StatusCode::NotFound) return Ok(CandidateId{0});
    return document.status;
  }

  // Parsed rather than trusted. This manager wrote it, but it went through a store that outlives
  // the process and can be edited by anything with the origin's storage — and `stoull` on a
  // non-number throws, which is not available here.
  const std::string& text = document.value;
  uint64_t chosen = 0;
  const auto* const end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, chosen);
  if (parsed.ec != std::errc{} || parsed.ptr != end || chosen == 0) {
    return Err<CandidateId>(StatusCode::Internal, kComponent,
                            "this project's selection for that cell is not a candidate identity");
  }
  return Ok(CandidateId{chosen});
}

Status ProjectManager::Export(ProjectId project, BuildId, const ExportSpec&) {
  if (!Exists(project)) return Fail(StatusCode::NotFound, kComponent, "no such project");
  return Fail(StatusCode::Unsupported, kComponent,
              "nothing to export until the build pipeline lands in Phase 2");
}

}  // namespace sphanorama
