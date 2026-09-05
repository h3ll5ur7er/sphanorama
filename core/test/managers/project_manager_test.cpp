// Project lifecycle. Everything that survives a killed tab goes through here, so "what happens to
// a project that was never created" is load-bearing rather than an edge case.
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "managers/project_manager/project_manager.h"
#include "support/fake_export_access.h"
#include "support/fake_project_store_access.h"

namespace sphanorama {
namespace {

// A store that answers a named document with a failure of the test's choosing.
//
// Every implementation of `IProjectStoreAccess` today refuses a missing document with `NotFound`
// and has no other way to fail, so the one case that matters here — a read that failed for some
// *other* reason — is not reachable against a real one. It is reachable through the contract, and
// a reader that folds every failure into "nobody has chosen here" would answer the ranking's pick
// to a storage error and say nothing to anybody.
class UnreadableProjectStoreAccess final : public IProjectStoreAccess {
 public:
  UnreadableProjectStoreAccess(IProjectStoreAccess& real, std::string key, StatusCode code)
      : real_(real), key_(std::move(key)), code_(code) {}

  Result<std::vector<ProjectId>> ListProjects() override { return real_.ListProjects(); }
  Result<std::string> ReadDocument(ProjectId project, std::string_view key) override {
    if (key == key_) return Err<std::string>(code_, "test", "this document will not come back");
    return real_.ReadDocument(project, key);
  }
  Status WriteDocument(ProjectId project, std::string_view key, std::string_view value) override {
    return real_.WriteDocument(project, key, value);
  }
  Status DeleteProject(ProjectId project) override { return real_.DeleteProject(project); }

 private:
  IProjectStoreAccess& real_;
  std::string key_;
  StatusCode code_;
};

class Projects : public ::testing::Test {
 protected:
  void SetUp() override {
    manager = std::make_unique<ProjectManager>(store);
  }
  FakeProjectStoreAccess store;
  FakeExportAccess exporter;
  std::unique_ptr<ProjectManager> manager;
};

TEST_F(Projects, ANewProjectAppearsInTheListing) {
  auto created = manager->Create("kitchen");
  ASSERT_TRUE(created.ok());
  auto listed = manager->List();
  ASSERT_TRUE(listed.ok());
  ASSERT_EQ(listed.value.size(), 1u);
  EXPECT_EQ(listed.value.front().id.value, created.value.value);
  EXPECT_EQ(listed.value.front().title, "kitchen");
}

TEST_F(Projects, ProjectsGetDistinctIdentities) {
  EXPECT_NE(manager->Create("a").value.value, manager->Create("b").value.value);
}

TEST_F(Projects, AnEmptyTitleIsRefused) {
  // An untitled project is indistinguishable from every other untitled project in the listing,
  // which is the one screen where a user has to tell them apart.
  EXPECT_EQ(manager->Create("").status.code, StatusCode::InvalidArgument);
}

TEST_F(Projects, ListingIsEmptyBeforeAnythingIsCreated) {
  auto listed = manager->List();
  ASSERT_TRUE(listed.ok());
  EXPECT_TRUE(listed.value.empty());
}

TEST_F(Projects, AProjectWithASessionDocumentIsOfferedBackToTheUser) {
  // The page cannot find out by trying: `Resume` reads its document before it touches anything,
  // but a *successful* one opens the camera and starts tracking, so probing would commit to a
  // resume nobody asked for. The listing is the side-effect-free answer (ADR 0036).
  auto interrupted = manager->Create("interrupted");
  auto untouched = manager->Create("untouched");
  ASSERT_TRUE(interrupted.ok());
  ASSERT_TRUE(untouched.ok());
  ASSERT_TRUE(store.WriteDocument(interrupted.value, "session", "sphanorama-session 1\n").ok());

  auto listed = manager->List();
  ASSERT_TRUE(listed.ok());
  ASSERT_EQ(listed.value.size(), 2u);
  for (const ProjectSummary& summary : listed.value) {
    EXPECT_EQ(summary.hasSession, summary.id.value == interrupted.value.value)
        << "project " << summary.id.value;
  }
}

TEST_F(Projects, TheSessionFlagIsReadOffTheStoreEveryTime) {
  // Not latched at Create, and not cached from the last listing. A session document appears while
  // this manager is alive — CaptureSessionManager checkpoints one on the way out of every burst —
  // so a listing answering from anything but the store would report a capture as unresumable for
  // as long as the tab that made it stayed open, which is exactly the tab that can resume it.
  auto created = manager->Create("kitchen");
  ASSERT_FALSE(manager->List().value.front().hasSession);

  ASSERT_TRUE(store.WriteDocument(created.value, "session", "sphanorama-session 1\n").ok());
  EXPECT_TRUE(manager->List().value.front().hasSession);

  // And a manager that never saw it written agrees, which is the case that actually happens: the
  // page that resumes is a new process reading a store that outlived the last one.
  ProjectManager reloaded(store);
  EXPECT_TRUE(reloaded.List().value.front().hasSession);
}

TEST_F(Projects, DeletingRemovesItFromTheListing) {
  auto created = manager->Create("kitchen");
  ASSERT_TRUE(manager->Delete(created.value).ok());
  EXPECT_TRUE(manager->List().value.empty());
}

TEST_F(Projects, DeletingSomethingThatDoesNotExistIsRefused) {
  EXPECT_EQ(manager->Delete(ProjectId{404}).code, StatusCode::NotFound);
}

TEST_F(Projects, SelectionIsRecordedAgainstTheProject) {
  // A manual candidate choice takes the same path as a retake: it marks the node dirty for the
  // next build (ADR 0004). Recording it is what makes that possible.
  auto created = manager->Create("kitchen");
  ASSERT_TRUE(manager->SetSelection(created.value, NodeId{3}, CandidateId{7}).ok());
  EXPECT_GT(store.WriteCount(), 1);
}

TEST_F(Projects, SelectionOnAnUnknownProjectIsRefused) {
  EXPECT_EQ(manager->SetSelection(ProjectId{404}, NodeId{1}, CandidateId{1}).code,
            StatusCode::NotFound);
}

TEST_F(Projects, AnUnsetIdIsNotASelection) {
  // The write path must not be able to create state the read path calls impossible. Zero is what
  // `GetSelection` answers for "nobody has chosen here", so a document *containing* zero is a
  // contradiction — and it was reachable, because `SetSelection` wrote whatever it was handed.
  // `Id::valid()` is `value != 0` and every counter here starts at 1, so an unset id is a caller
  // mistake rather than a choice.
  auto created = manager->Create("kitchen");
  EXPECT_EQ(manager->SetSelection(created.value, NodeId{3}, CandidateId{0}).code,
            StatusCode::InvalidArgument);
  EXPECT_EQ(manager->SetSelection(created.value, NodeId{0}, CandidateId{7}).code,
            StatusCode::InvalidArgument);

  // And nothing was written: the cell still reads as one nobody has chosen for, rather than as a
  // document this manager cannot parse.
  auto chosen = manager->GetSelection(created.value, NodeId{3});
  EXPECT_TRUE(chosen.ok()) << chosen.status.detail;
  EXPECT_EQ(chosen.value.value, 0u);
}

TEST_F(Projects, AskingAboutAnUnsetCellIsACallerMistakeRatherThanAnEmptyAnswer) {
  // The mirror of the rule above. `SetSelection` refuses an unset cell, so `selection/0` is a key
  // nothing can ever write — and answering "nobody has chosen here" for it would make a caller
  // passing an uninitialised id indistinguishable from a cell that genuinely has no override.
  // The sentinel is worth protecting: it only means something because every other answer is a
  // real one.
  auto created = manager->Create("kitchen");
  auto asked = manager->GetSelection(created.value, NodeId{0});
  EXPECT_FALSE(asked.ok());
  EXPECT_EQ(asked.status.code, StatusCode::InvalidArgument);

  // A real cell with nothing recorded still answers zero, so this cannot pass by refusing every
  // cell that has not been chosen for.
  auto empty = manager->GetSelection(created.value, NodeId{9});
  EXPECT_TRUE(empty.ok()) << empty.status.detail;
  EXPECT_EQ(empty.value.value, 0u);
}

TEST_F(Projects, AReadThatFailedIsNotACellNobodyHasChosenFor) {
  // Absence is the sentinel; a failure is not absence. Folding every refusal into zero would show
  // the ranking's pick for a cell whose override could not be read — the screen quietly
  // disagreeing with the build, which is the failure this whole call exists to end — and it would
  // do it without a word in the logs.
  auto created = manager->Create("kitchen");
  ASSERT_TRUE(manager->SetSelection(created.value, NodeId{3}, CandidateId{7}).ok());

  UnreadableProjectStoreAccess broken{store, "selection/3", StatusCode::Internal};
  ProjectManager reader{broken};
  auto chosen = reader.GetSelection(created.value, NodeId{3});
  EXPECT_FALSE(chosen.ok()) << "a store that could not answer was read as an unchosen cell";
  EXPECT_EQ(chosen.status.code, StatusCode::Internal);

  // And a genuinely missing document still answers zero through the same store, so this cannot
  // pass by refusing every read.
  auto empty = reader.GetSelection(created.value, NodeId{4});
  EXPECT_TRUE(empty.ok()) << empty.status.detail;
  EXPECT_EQ(empty.value.value, 0u);
}

TEST_F(Projects, ARecordedSelectionCanBeReadBack) {
  // The whole point, and what was missing: a pick was written here and read nowhere, so the only
  // thing that knew which candidate was in force was the client that had just set it — and a
  // reload forgets that. The build reads this document; now so can whoever has to show it.
  auto created = manager->Create("kitchen");
  ASSERT_TRUE(manager->SetSelection(created.value, NodeId{3}, CandidateId{7}).ok());

  auto chosen = manager->GetSelection(created.value, NodeId{3});
  ASSERT_TRUE(chosen.ok()) << chosen.status.detail;
  EXPECT_EQ(chosen.value.value, 7u);
}

TEST_F(Projects, ACellNobodyHasChosenForAnswersZeroRatherThanFailing) {
  // Zero is a real answer here, not an absence dressed as one: `Id::valid()` is `value != 0` and
  // every counter in these contracts starts at 1, so no selection can be zero.
  //
  // It matters that this is not `NotFound`. A client reads a Result's status to tell a call that
  // failed from one that worked, so folding "nobody has chosen here" into the failure branch
  // would make an unreadable project look exactly like an unedited one — and the review strip
  // would show the automatic pick in force either way, which is right for one and a lie for the
  // other.
  auto created = manager->Create("kitchen");
  auto chosen = manager->GetSelection(created.value, NodeId{9});
  ASSERT_TRUE(chosen.ok()) << chosen.status.detail;
  EXPECT_EQ(chosen.value.value, 0u);
  EXPECT_FALSE(chosen.value.valid());
}

TEST_F(Projects, ASelectionIsPerCellAndPerProject) {
  // One key per node, and the key carries the project. Reading a cell's pick must not answer with
  // its neighbour's, and two projects that both chose something for cell 3 must not share it.
  auto kitchen = manager->Create("kitchen");
  auto garden = manager->Create("garden");
  ASSERT_TRUE(manager->SetSelection(kitchen.value, NodeId{3}, CandidateId{7}).ok());
  ASSERT_TRUE(manager->SetSelection(kitchen.value, NodeId{4}, CandidateId{11}).ok());
  ASSERT_TRUE(manager->SetSelection(garden.value, NodeId{3}, CandidateId{2}).ok());

  EXPECT_EQ(manager->GetSelection(kitchen.value, NodeId{3}).value.value, 7u);
  EXPECT_EQ(manager->GetSelection(kitchen.value, NodeId{4}).value.value, 11u);
  EXPECT_EQ(manager->GetSelection(garden.value, NodeId{3}).value.value, 2u);
}

TEST_F(Projects, TheLastChoiceForACellIsTheOneThatAnswers) {
  // Changing a pick overwrites rather than accumulating, which is what a user pressing a second
  // thumbnail means.
  auto created = manager->Create("kitchen");
  ASSERT_TRUE(manager->SetSelection(created.value, NodeId{3}, CandidateId{7}).ok());
  ASSERT_TRUE(manager->SetSelection(created.value, NodeId{3}, CandidateId{8}).ok());
  EXPECT_EQ(manager->GetSelection(created.value, NodeId{3}).value.value, 8u);
}

TEST_F(Projects, ASelectionDocumentThatIsNotACandidateIsRefusedRatherThanGuessed) {
  // Written underneath the manager, because no contract writes this shape — which is the point.
  // The document went through a store that outlives the process and lives in storage anything
  // with the origin can edit, so "this manager wrote it" is not a reason to trust what comes
  // back. A partial parse is the trap worth naming: "7x" would answer 7 to anything that stopped
  // reading at the first non-digit, and the pick shown would be a candidate nobody chose.
  auto created = manager->Create("kitchen");
  for (const char* garbage : {"", "  ", "seven", "7x", "-1", "0", "9999999999999999999999"}) {
    ASSERT_TRUE(store.WriteDocument(created.value, "selection/3", garbage).ok()) << garbage;
    auto chosen = manager->GetSelection(created.value, NodeId{3});
    EXPECT_FALSE(chosen.ok()) << "\"" << garbage << "\" was read as a selection";
    EXPECT_EQ(chosen.status.code, StatusCode::Internal) << garbage;
  }

  // And a real one still reads, so this cannot pass by refusing everything.
  ASSERT_TRUE(manager->SetSelection(created.value, NodeId{3}, CandidateId{7}).ok());
  EXPECT_EQ(manager->GetSelection(created.value, NodeId{3}).value.value, 7u);
}

TEST_F(Projects, ReadingASelectionFromAnUnknownProjectIsRefused) {
  // The same answer `SetSelection` gives, and for the same reason: a project that does not exist
  // is a failure rather than a cell nobody has chosen for.
  auto chosen = manager->GetSelection(ProjectId{404}, NodeId{1});
  EXPECT_FALSE(chosen.ok());
  EXPECT_EQ(chosen.status.code, StatusCode::NotFound);
}

TEST_F(Projects, ExportingABuildThatDoesNotExistIsRefused) {
  // There is no build pipeline yet, so this is the honest answer rather than a file full of
  // nothing.
  auto created = manager->Create("kitchen");
  ExportSpec spec;
  spec.filename = "sphere.jpg";
  const Status status = manager->Export(created.value, BuildId{1}, spec);
  EXPECT_FALSE(status.ok());
  EXPECT_TRUE(exporter.artifacts().empty());
}

TEST_F(Projects, IdsDoNotCollideWithProjectsThatOutlivedTheManager) {
  // The manager is rebuilt on every page load; the store is not. A counter that restarts at 1
  // hands the next project an id that is already taken, and WriteDocument then overwrites a real
  // project's title instead of creating a new one — silent data loss, one reload later.
  auto first = manager->Create("kitchen");
  auto second = manager->Create("garden");
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());

  ProjectManager reloaded(store);
  auto third = reloaded.Create("balcony");
  ASSERT_TRUE(third.ok());
  EXPECT_NE(third.value.value, first.value.value);
  EXPECT_NE(third.value.value, second.value.value);

  auto listed = reloaded.List();
  ASSERT_TRUE(listed.ok());
  EXPECT_EQ(listed.value.size(), 3u);
}

}  // namespace
}  // namespace sphanorama
