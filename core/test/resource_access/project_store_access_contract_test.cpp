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

}  // namespace
}  // namespace sphanorama
