#include "resource_access/frame_store_access/null_frame_store_access.h"

namespace sphanorama {
namespace {
constexpr const char* kComponent = "NullFrameStoreAccess";
constexpr const char* kReason = "no frame store: the tiered store lands in Phase 1";
}  // namespace

Result<FrameStoreBudget> NullFrameStoreAccess::Budget() { return Ok(FrameStoreBudget{}); }
Result<FrameRef> NullFrameStoreAccess::Allocate(int32_t, int32_t, PixelFormat) {
  return Err<FrameRef>(StatusCode::FrameStoreExhausted, kComponent, kReason);
}
Result<std::span<uint8_t>> NullFrameStoreAccess::Pin(const FrameRef&) {
  return Err<std::span<uint8_t>>(StatusCode::NotFound, kComponent, kReason);
}
Status NullFrameStoreAccess::Release(const FrameRef&) {
  return Fail(StatusCode::NotFound, kComponent, kReason);
}
Result<Residency> NullFrameStoreAccess::ResidencyOf(const FrameRef&) {
  return Err<Residency>(StatusCode::NotFound, kComponent, kReason);
}
Status NullFrameStoreAccess::Demote(const FrameRef&, Residency) {
  return Fail(StatusCode::NotFound, kComponent, kReason);
}
Status NullFrameStoreAccess::Forget(const FrameRef&) {
  return Fail(StatusCode::NotFound, kComponent, kReason);
}
Result<uint64_t> NullFrameStoreAccess::ContentHash(const FrameRef&) {
  return Err<uint64_t>(StatusCode::NotFound, kComponent, kReason);
}

}  // namespace sphanorama
