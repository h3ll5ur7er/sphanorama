#include "resource_access/camera_access/null_camera_access.h"

namespace sphanorama {
namespace {
constexpr const char* kComponent = "NullCameraAccess";
constexpr const char* kReason = "no camera port: the browser adapter is not wired to the core yet";
}  // namespace

Result<CameraCapabilities> NullCameraAccess::Open(const CameraOpenSpec&) {
  return Err<CameraCapabilities>(StatusCode::CameraUnavailable, kComponent, kReason);
}
Status NullCameraAccess::StartPreview() {
  return Fail(StatusCode::CameraUnavailable, kComponent, kReason);
}
Status NullCameraAccess::StopPreview() { return Status::Ok(); }
Result<FrameRef> NullCameraAccess::PeekPreviewFrame() {
  return Err<FrameRef>(StatusCode::CameraUnavailable, kComponent, kReason);
}
Result<std::vector<FrameRef>> NullCameraAccess::CaptureBurst(const BurstSpec&) {
  return Err<std::vector<FrameRef>>(StatusCode::CameraUnavailable, kComponent, kReason);
}
Status NullCameraAccess::SetLocks(bool, bool, bool) {
  return Fail(StatusCode::CameraUnavailable, kComponent, kReason);
}
Status NullCameraAccess::Close() { return Status::Ok(); }

}  // namespace sphanorama
