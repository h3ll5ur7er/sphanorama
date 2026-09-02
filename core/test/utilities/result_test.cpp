// The error model is load-bearing: no exceptions cross a layer boundary, so every fallible call
// returns Result<T>. These tests pin the ergonomics that make that tolerable to write.
#include <gtest/gtest.h>

#include "sphanorama/types.h"

namespace sphanorama {
namespace {

Result<int> Succeeds() { return Ok(7); }
Result<int> Fails() { return Err<int>(StatusCode::NotFound, "test", "no such thing"); }

TEST(Result, OkCarriesTheValueAndAnOkStatus) {
  auto r = Succeeds();
  EXPECT_TRUE(r.ok());
  EXPECT_EQ(r.value, 7);
  EXPECT_EQ(r.status.code, StatusCode::Ok);
}

TEST(Result, ErrCarriesTheCodeComponentAndDetail) {
  auto r = Fails();
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.status.code, StatusCode::NotFound);
  EXPECT_STREQ(r.status.component, "test");
  EXPECT_EQ(r.status.detail, "no such thing");
}

TEST(Result, AFailedStatusConvertsIntoAnyResultType) {
  // This is what lets SPH_TRY propagate a failure out of a function with a different value type.
  Result<double> r = Fail(StatusCode::Cancelled, "test");
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.status.code, StatusCode::Cancelled);
}

Result<double> DoubleIt() {
  SPH_TRY(auto v, Succeeds());
  return Ok(v * 2.0);
}

Result<double> DoubleItButFails() {
  SPH_TRY(auto v, Fails());
  return Ok(v * 2.0);
}

Status DiscardIt() {
  SPH_TRY(auto v, Fails());
  (void)v;
  return Status::Ok();
}

TEST(SphTry, UnwrapsOnSuccess) {
  auto r = DoubleIt();
  ASSERT_TRUE(r.ok());
  EXPECT_DOUBLE_EQ(r.value, 14.0);
}

TEST(SphTry, PropagatesTheOriginalStatusOnFailure) {
  auto r = DoubleItButFails();
  EXPECT_FALSE(r.ok());
  EXPECT_EQ(r.status.code, StatusCode::NotFound);
  EXPECT_EQ(r.status.detail, "no such thing");
}

TEST(SphTry, WorksInAStatusReturningFunctionToo) {
  EXPECT_EQ(DiscardIt().code, StatusCode::NotFound);
}

TEST(SphTry, TwoUsesInOneScopeDoNotCollide) {
  auto f = []() -> Result<int> {
    SPH_TRY(auto a, Succeeds());
    SPH_TRY(auto b, Succeeds());
    return Ok(a + b);
  };
  auto r = f();
  ASSERT_TRUE(r.ok());
  EXPECT_EQ(r.value, 14);
}

TEST(Id, DistinctTagsAreDistinctTypes) {
  // A NodeId must never be passable where a FrameId is expected; if this ever compiles as a
  // direct assignment the strong typedef has been weakened.
  static_assert(!std::is_convertible_v<NodeId, FrameId>);
  NodeId n{4};
  EXPECT_TRUE(n.valid());
  EXPECT_FALSE(NodeId{}.valid());
}

}  // namespace
}  // namespace sphanorama
