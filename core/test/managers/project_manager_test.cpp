// Project lifecycle. Everything that survives a killed tab goes through here, so "what happens to
// a project that was never created" is load-bearing rather than an edge case.
#include <gtest/gtest.h>

#include <memory>

#include "managers/project_manager/project_manager.h"
#include "support/fake_export_access.h"
#include "support/fake_project_store_access.h"

namespace sphanorama {
namespace {

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
