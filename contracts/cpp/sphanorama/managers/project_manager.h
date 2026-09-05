#pragma once
#include <string_view>
#include "sphanorama/types.h"

namespace sphanorama {

// V3 — project lifecycle and export. The only manager that touches IExportAccess.
// @boundary @facade
class IProjectManager {
 public:
  virtual ~IProjectManager() = default;

  // No Resume here, deliberately. Picking a capture back up means handing a live session to
  // whatever owns session state, and that is `ICaptureSessionManager` — this manager could only
  // ever have returned a SessionId it had no way to make (ADR 0029).
  //
  // `hasSession` is not that method coming back. It reports that a session document exists, which
  // is metadata about a project and nothing a caller could mistake for a session: there is no
  // SessionId in a summary, and nothing here parses the document or hands back what is in it. It
  // is what lets a page offer a resume rather than discover one by attempting it (ADR 0036).
  virtual Result<std::vector<ProjectSummary>> List() = 0;
  virtual Result<ProjectId> Create(std::string_view title) = 0;
  virtual Status Delete(ProjectId project) = 0;

  // A manual override of automatic burst selection. Marks the node dirty for the next build, so
  // it takes exactly the same path as a retake.
  virtual Status SetSelection(ProjectId project, NodeId node, CandidateId candidate) = 0;

  // What was chosen for a cell, or nothing.
  //
  // The counterpart of `SetSelection`, and it exists because nothing else could answer: a pick was
  // written here and read nowhere, so a review client's only way to show which candidate was in
  // force was to remember its own writes — which a reload forgets, along with the choice the user
  // had just made.
  //
  // **A zero candidate means nobody has chosen here, and it is a success.** `Id::valid()` is
  // `value != 0` and every counter in these contracts starts at 1, so zero is a value no selection
  // can have. It is deliberately not `NotFound`: a caller reads a Result's status to tell a call
  // that failed from one that worked, and folding "no override" into the failure branch would make
  // a project this build cannot read look exactly like one nobody has edited. A project that does
  // not exist is still a failure, because that is a question about the project rather than an
  // answer about the cell.
  virtual Result<CandidateId> GetSelection(ProjectId project, NodeId node) = 0;

  virtual Status Export(ProjectId project, BuildId build, const ExportSpec& spec) = 0;
};

}  // namespace sphanorama
