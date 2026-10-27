// The project-store contract. Everything a session needs to survive a killed tab goes through
// here, so "what happens to a key that was never written" is a load-bearing question.
#include <gtest/gtest.h>

#include <memory>

#include "sphanorama/resource_access/project_store_access.h"
#include "resource_access/project_store_access/memory_project_store_access.h"
#include "support/fake_project_store_access.h"

namespace sphanorama {
namespace {

constexpr ProjectId kProject{7};
constexpr ProjectId kOther{8};

template <typename Factory>
class ProjectStoreAccessContract : public ::testing::Test {
 protected:
  std::unique_ptr<IProjectStoreAccess> store = Factory::Create();
};

struct MemoryProjectStoreAccessFactory {
  static std::unique_ptr<IProjectStoreAccess> Create() {
    return std::make_unique<MemoryProjectStoreAccess>();
  }
};

// Two implementations now, which is the point of a shared suite: the property that matters is
// that they agree, and adding one is a line here rather than a second test file (ADR 0010).
using Implementations = ::testing::Types<FakeProjectStoreAccessFactory,
                                         MemoryProjectStoreAccessFactory>;
TYPED_TEST_SUITE(ProjectStoreAccessContract, Implementations);

TYPED_TEST(ProjectStoreAccessContract, ADocumentComesBackAsItWasWritten) {
  ASSERT_TRUE(this->store->WriteDocument(kProject, "plan", R"({"nodes":48})").ok());
  auto read = this->store->ReadDocument(kProject, "plan");
  ASSERT_TRUE(read.ok());
  EXPECT_EQ(read.value, R"({"nodes":48})");
}

TYPED_TEST(ProjectStoreAccessContract, AMissingDocumentIsNotFoundRatherThanEmpty) {
  // An empty string would be indistinguishable from a document that was written empty, and a
  // resume path would silently start from a blank plan.
  ASSERT_TRUE(this->store->WriteDocument(kProject, "plan", "{}").ok());
  EXPECT_EQ(this->store->ReadDocument(kProject, "absent").status.code, StatusCode::NotFound);
}

TYPED_TEST(ProjectStoreAccessContract, ReadingFromAnUnknownProjectIsNotFound) {
  EXPECT_EQ(this->store->ReadDocument(kProject, "plan").status.code, StatusCode::NotFound);
}

TYPED_TEST(ProjectStoreAccessContract, WritingTwiceReplaces) {
  ASSERT_TRUE(this->store->WriteDocument(kProject, "plan", "first").ok());
  ASSERT_TRUE(this->store->WriteDocument(kProject, "plan", "second").ok());
  EXPECT_EQ(this->store->ReadDocument(kProject, "plan").value, "second");
}

TYPED_TEST(ProjectStoreAccessContract, ProjectsAreIsolatedFromOneAnother) {
  ASSERT_TRUE(this->store->WriteDocument(kProject, "plan", "mine").ok());
  ASSERT_TRUE(this->store->WriteDocument(kOther, "plan", "theirs").ok());
  EXPECT_EQ(this->store->ReadDocument(kProject, "plan").value, "mine");
  EXPECT_EQ(this->store->ReadDocument(kOther, "plan").value, "theirs");
}

TYPED_TEST(ProjectStoreAccessContract, ListingReportsEveryProjectWrittenExactlyOnce) {
  ASSERT_TRUE(this->store->WriteDocument(kProject, "plan", "a").ok());
  ASSERT_TRUE(this->store->WriteDocument(kProject, "nodes", "b").ok());
  ASSERT_TRUE(this->store->WriteDocument(kOther, "plan", "c").ok());
  auto projects = this->store->ListProjects();
  ASSERT_TRUE(projects.ok());
  EXPECT_EQ(projects.value.size(), 2u);
}

TYPED_TEST(ProjectStoreAccessContract, ListingIsEmptyBeforeAnythingIsWritten) {
  auto projects = this->store->ListProjects();
  ASSERT_TRUE(projects.ok());
  EXPECT_TRUE(projects.value.empty());
}

TYPED_TEST(ProjectStoreAccessContract, DeletingRemovesEveryDocumentOfThatProject) {
  ASSERT_TRUE(this->store->WriteDocument(kProject, "plan", "a").ok());
  ASSERT_TRUE(this->store->WriteDocument(kProject, "nodes", "b").ok());
  ASSERT_TRUE(this->store->DeleteProject(kProject).ok());
  EXPECT_EQ(this->store->ReadDocument(kProject, "plan").status.code, StatusCode::NotFound);
  EXPECT_EQ(this->store->ReadDocument(kProject, "nodes").status.code, StatusCode::NotFound);
}

TYPED_TEST(ProjectStoreAccessContract, DeletingLeavesOtherProjectsAlone) {
  ASSERT_TRUE(this->store->WriteDocument(kProject, "plan", "a").ok());
  ASSERT_TRUE(this->store->WriteDocument(kOther, "plan", "b").ok());
  ASSERT_TRUE(this->store->DeleteProject(kProject).ok());
  EXPECT_EQ(this->store->ReadDocument(kOther, "plan").value, "b");
}

TYPED_TEST(ProjectStoreAccessContract, DeletingAnUnknownProjectIsNotFound) {
  EXPECT_EQ(this->store->DeleteProject(kProject).code, StatusCode::NotFound);
}

TYPED_TEST(ProjectStoreAccessContract, RejectsAnUnsetProjectOrEmptyKey) {
  EXPECT_EQ(this->store->WriteDocument(ProjectId{}, "plan", "x").code,
            StatusCode::InvalidArgument);
  EXPECT_EQ(this->store->WriteDocument(kProject, "", "x").code, StatusCode::InvalidArgument);
}

TYPED_TEST(ProjectStoreAccessContract, AnIssuedIdIsOneNoProjectHolds) {
  ASSERT_TRUE(this->store->WriteDocument(kProject, "title", "a").ok());
  ASSERT_TRUE(this->store->WriteDocument(kOther, "title", "b").ok());
  auto issued = this->store->IssueProjectId();
  ASSERT_TRUE(issued.ok());
  EXPECT_TRUE(issued.value.valid());
  EXPECT_NE(issued.value.value, kProject.value);
  EXPECT_NE(issued.value.value, kOther.value);
}

TYPED_TEST(ProjectStoreAccessContract, IdsAscendInTheOrderTheyWereIssued) {
  // How a page tells the newest capture, which is the only one whose frames the tier holds.
  auto first = this->store->IssueProjectId();
  auto second = this->store->IssueProjectId();
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_GT(second.value.value, first.value.value);
}

TYPED_TEST(ProjectStoreAccessContract, IssuingAnIdCreatesNothing) {
  ASSERT_TRUE(this->store->IssueProjectId().ok());
  EXPECT_TRUE(this->store->ListProjects().value.empty());
}

TYPED_TEST(ProjectStoreAccessContract, AnIdIsNotIssuedAgainOnceItsProjectIsDeleted) {
  // A deleted project's id names nothing on disk, but a document written against it may still
  // name it somewhere else, and "never issued before" is the promise that does not need checking.
  auto first = this->store->IssueProjectId();
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(this->store->WriteDocument(first.value, "title", "a").ok());
  ASSERT_TRUE(this->store->DeleteProject(first.value).ok());
  auto second = this->store->IssueProjectId();
  ASSERT_TRUE(second.ok());
  EXPECT_GT(second.value.value, first.value.value);
}

TYPED_TEST(ProjectStoreAccessContract, AnIdIsNotIssuedAtOrBelowOneWrittenWithoutBeingIssued) {
  // A write creates on demand, so a project can exist under an id this store never issued: one
  // an older build counted out, or one written before the store held the listing it has now.
  ASSERT_TRUE(this->store->WriteDocument(ProjectId{40}, "title", "a").ok());
  ASSERT_TRUE(this->store->DeleteProject(ProjectId{40}).ok());
  auto issued = this->store->IssueProjectId();
  ASSERT_TRUE(issued.ok());
  EXPECT_GT(issued.value.value, 40u);
}

TYPED_TEST(ProjectStoreAccessContract, NoIdIsIssuedThatADoubleCannotHold) {
  // Every id crosses to the page as a JavaScript number, where one past 2^53 is the same number
  // as its neighbour. Refused rather than issued, because two projects under one key is the
  // overwrite this method exists to prevent.
  constexpr uint64_t kLargestExact = (uint64_t{1} << 53) - 1;
  ASSERT_TRUE(this->store->WriteDocument(ProjectId{kLargestExact - 1}, "title", "a").ok());
  auto last = this->store->IssueProjectId();
  ASSERT_TRUE(last.ok());
  EXPECT_EQ(last.value.value, kLargestExact);
  EXPECT_EQ(this->store->IssueProjectId().status.code, StatusCode::FailedPrecondition);
}

}  // namespace
}  // namespace sphanorama
