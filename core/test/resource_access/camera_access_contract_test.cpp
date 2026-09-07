// The camera contract. Ordering is the whole of it: a capture session drives open -> preview ->
// burst -> close, and every implementation has to refuse the same out-of-order calls rather than
// returning something plausible.
#include <gtest/gtest.h>

#include <memory>
#include <set>

#include "sphanorama/resource_access/camera_access.h"
#include "support/fake_camera_access.h"
#include "resource_access/camera_access/null_camera_access.h"

namespace sphanorama {
namespace {

template <typename Factory>
class CameraAccessContract : public ::testing::Test {
 protected:
  std::unique_ptr<ICameraAccess> camera = Factory::Create();

  void Open() { ASSERT_TRUE(camera->Open(CameraOpenSpec{}).ok()); }
};

// One implementation, and it is a fake — which is a limit of this suite worth stating rather than
// leaving to be discovered. `NullCameraAccess` cannot join it (refusing everything that needs a
// camera is its whole job, and it has its own test below — which is where `StopPreview` and
// `Close` answering `Ok` is asserted, and why this does not say "refuses every call", a phrase
// this branch has now had to correct in four places), and the bridge's browser port cannot either: it is written in terms the
// host provides, so it compiles and runs under the web build and nowhere else. Everything this
// suite says about that implementation is therefore said by analogy.
//
// What holds that port instead is the browser suite. `the camera the core paces a burst by is the
// one the locks left behind` drives `Open`, `SetLocks` and `Capabilities` through the real web
// build against a camera whose frame rate changes when its exposure is pinned — which is the one
// arrangement in the tree where an implementation returning `Ok(CameraCapabilities{})` from
// `Capabilities()` goes red.
using Implementations = ::testing::Types<FakeCameraAccessFactory>;
TYPED_TEST_SUITE(CameraAccessContract, Implementations);

TYPED_TEST(CameraAccessContract, OpenReportsWhatTheLensCanDo) {
  auto caps = this->camera->Open(CameraOpenSpec{});
  ASSERT_TRUE(caps.ok());
  EXPECT_GT(caps.value.maxWidth, 0);
  EXPECT_GT(caps.value.maxHeight, 0);
}

TYPED_TEST(CameraAccessContract, PreviewBeforeOpenIsRefused) {
  EXPECT_EQ(this->camera->StartPreview().code, StatusCode::FailedPrecondition);
}

TYPED_TEST(CameraAccessContract, CapabilitiesBeforeOpenIsRefused) {
  // Added a round after the method was. Three implementations gave three answers for this one
  // state — `Ok` with a fixture's numbers, `FailedPrecondition`, and `CameraUnavailable` — which
  // is what a contract exists to stop, and there was no line here holding any of them to it.
  EXPECT_EQ(this->camera->Capabilities().status.code, StatusCode::FailedPrecondition);
}

TYPED_TEST(CameraAccessContract, CapabilitiesAgreesWithWhatOpenReported) {
  // The two calls answer the same question about the same device, so on a camera nothing has
  // changed they cannot differ. `BrowserCameraAccess` shares one reading between them for this
  // reason (ADR 0045).
  //
  // Against the fake this is close to tautological — one member answers both calls, which is what
  // makes the fake a *correct* implementation of the rule and also what stops this line being
  // evidence about anybody else's. It is here as the statement of the requirement; the
  // implementation it is really about is held by the browser suite, per the note on
  // `Implementations` above.
  auto opened = this->camera->Open(CameraOpenSpec{});
  ASSERT_TRUE(opened.ok());
  auto asked = this->camera->Capabilities();
  ASSERT_TRUE(asked.ok()) << asked.status.detail;
  // Not only equal to `opened` but a real answer, so an implementation that answered
  // `Ok(CameraCapabilities{})` from both fails here rather than agreeing with itself about nothing.
  EXPECT_GT(asked.value.maxWidth, 0);
  EXPECT_GT(asked.value.maxHeight, 0);
  EXPECT_EQ(asked.value.maxWidth, opened.value.maxWidth);
  EXPECT_EQ(asked.value.maxHeight, opened.value.maxHeight);
  EXPECT_DOUBLE_EQ(asked.value.maxBurstFps, opened.value.maxBurstFps);
  EXPECT_EQ(asked.value.supportsExposureLock, opened.value.supportsExposureLock);
}

TYPED_TEST(CameraAccessContract, CapabilitiesAfterCloseIsRefusedAgain) {
  // Closing returns the port to its initial state, which the suite already asserts for the other
  // calls. A capability read that kept answering would report a camera nobody is holding.
  this->Open();
  ASSERT_TRUE(this->camera->Close().ok());
  EXPECT_EQ(this->camera->Capabilities().status.code, StatusCode::FailedPrecondition);
}

TYPED_TEST(CameraAccessContract, PeekBeforeOpenIsRefused) {
  EXPECT_EQ(this->camera->PeekPreviewFrame().status.code, StatusCode::FailedPrecondition);
}

TYPED_TEST(CameraAccessContract, PeekBeforePreviewIsRefused) {
  this->Open();
  EXPECT_EQ(this->camera->PeekPreviewFrame().status.code, StatusCode::FailedPrecondition);
}

TYPED_TEST(CameraAccessContract, PeekWorksOncePreviewIsRunning) {
  this->Open();
  ASSERT_TRUE(this->camera->StartPreview().ok());
  auto frame = this->camera->PeekPreviewFrame();
  ASSERT_TRUE(frame.ok());
  EXPECT_TRUE(frame.value.id.valid());
}

TYPED_TEST(CameraAccessContract, RepeatedPeeksReturnDistinctFrames) {
  // Since ADR 0018 a burst is several peeks, so this is the property the burst rests on: peeks
  // that aliased one another would make selection meaningless while still passing every
  // count-based check the manager could make.
  this->Open();
  ASSERT_TRUE(this->camera->StartPreview().ok());
  std::set<uint64_t> ids;
  for (int i = 0; i < 4; ++i) {
    auto frame = this->camera->PeekPreviewFrame();
    ASSERT_TRUE(frame.ok()) << frame.status.detail;
    ids.insert(frame.value.id.value);
  }
  EXPECT_EQ(ids.size(), 4u);
}

TYPED_TEST(CameraAccessContract, LockingBeforeOpenIsRefused) {
  EXPECT_EQ(this->camera->SetLocks(true, true, true).code, StatusCode::FailedPrecondition);
}

TYPED_TEST(CameraAccessContract, ClosingReturnsTheCameraToItsInitialState) {
  this->Open();
  ASSERT_TRUE(this->camera->Close().ok());
  EXPECT_EQ(this->camera->PeekPreviewFrame().status.code, StatusCode::FailedPrecondition);
}

// Not part of the shared suite: only the fake exposes what the session asked it to do.
// The null camera's refusals, which nothing else asserts any more.
//
// They had exactly one test: the facade suite's `StartingACaptureSessionFailsHonestlyWithoutACamera`,
// which drove `CaptureSessionManager::begin` through the native runtime and got `CameraUnavailable`
// back from this port. ADR 0044 put a sensor check in front of that call, so the same test now
// stops at `SensorUnavailable` and never reaches a camera at all — and a null port whose refusals
// nothing checks is one that can start answering `Ok` with an empty struct without anything going
// red. That is the failure null-over-stub exists to prevent, so the coverage moves here rather
// than disappearing.
TEST(NullCamera, RefusesEveryCallThatNeedsACameraWithAReasonRatherThanAnEmptyAnswer) {
  // Named for what it does. It was `RefusesEveryCall…`, which is not what this port does and not
  // what this test asserts: `StopPreview` and `Close` answer `Ok`, and the assertions at the end
  // are here so that "every call that needs a camera" is a partition of the port rather than a
  // phrase. The same sentence was wrong in `camera_access.h` and twice in ADR 0045; a reviewer
  // found it in the header, the fix reached the header alone, and a second reviewer found the rest.
  NullCameraAccess camera;

  auto opened = camera.Open(CameraOpenSpec{});
  EXPECT_FALSE(opened.ok());
  EXPECT_EQ(opened.status.code, StatusCode::CameraUnavailable);
  EXPECT_FALSE(opened.status.detail.empty()) << "a refusal with no reason is not an explanation";

  EXPECT_EQ(camera.StartPreview().code, StatusCode::CameraUnavailable);
  EXPECT_EQ(camera.PeekPreviewFrame().status.code, StatusCode::CameraUnavailable);
  EXPECT_EQ(camera.SetLocks(true, true, true).code, StatusCode::CameraUnavailable);
  // `CameraUnavailable` rather than `FailedPrecondition`, and that is the contract rather than an
  // inconsistency: this port has no camera at all, which is not a call made out of order.
  EXPECT_EQ(camera.Capabilities().status.code, StatusCode::CameraUnavailable);

  // And the two that do not refuse, which is the other half of the same rule. Stopping a preview
  // that is not running and closing a camera that is not open are requests this port has already
  // satisfied, so refusing them would make a caller's cleanup path report a failure that is not
  // one — `CaptureSessionManager::End` calls both and would carry the status out.
  EXPECT_TRUE(camera.StopPreview().ok());
  EXPECT_TRUE(camera.Close().ok());
}

TEST(FakeCamera, ARefusedLockWriteChangesNothing) {
  // The fake's own "not open means nothing happened", which the reordered guard in `SetLocks` is
  // for and which nothing asserted — the guard sat below the mutation it was meant to guard, so a
  // lock write to a closed camera changed what the camera would report once reopened while
  // returning a refusal saying it had done nothing.
  //
  // Not reachable from the manager, which never writes locks to a closed camera. It is asserted
  // because a fake that quietly does something on a path it says it refuses is a fake that will
  // one day explain a test result nobody can reproduce.
  FakeCameraAccess camera;
  camera.SlowToOnLock(2.0);

  EXPECT_EQ(camera.SetLocks(true, true, true).code, StatusCode::FailedPrecondition);

  ASSERT_TRUE(camera.Open(CameraOpenSpec{}).ok());
  EXPECT_DOUBLE_EQ(camera.Capabilities().value.maxBurstFps, 30.0)
      << "a lock write this camera refused still slowed it down";
}

TEST(FakeCamera, RecordsThatTheSessionLockedExposureForABurst) {
  FakeCameraAccess camera;
  ASSERT_TRUE(camera.Open(CameraOpenSpec{}).ok());
  ASSERT_TRUE(camera.SetLocks(true, true, true).ok());
  EXPECT_TRUE(camera.ExposureLocked());
}

TEST(FakeCamera, PeekedFramesCarryDistinctPixelsSoSelectionIsObservable) {
  FakeCameraAccess camera;
  ASSERT_TRUE(camera.Open(CameraOpenSpec{}).ok());
  ASSERT_TRUE(camera.StartPreview().ok());
  std::vector<FrameRef> taken;
  for (int i = 0; i < 3; ++i) {
    auto frame = camera.PeekPreviewFrame();
    ASSERT_TRUE(frame.ok());
    taken.push_back(frame.value);
  }

  std::set<uint64_t> hashes;
  for (const auto& frame : taken) {
    auto hash = camera.store()->ContentHash(frame);
    ASSERT_TRUE(hash.ok());
    hashes.insert(hash.value);
  }
  EXPECT_EQ(hashes.size(), 3u);
}

}  // namespace
}  // namespace sphanorama
