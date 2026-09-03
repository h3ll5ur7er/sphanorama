#include "managers/panorama_build_manager/panorama_build_manager.h"

namespace sphanorama {
namespace {
constexpr const char* kComponent = "PanoramaBuildManager";
constexpr const char* kNotYet = "the build pipeline lands in Phase 2";
}  // namespace

Result<BuildId> PanoramaBuildManager::Start(SessionId, const BuildSpec&) {
  return Err<BuildId>(StatusCode::Unsupported, kComponent, kNotYet);
}

Result<BuildProgress> PanoramaBuildManager::Poll(BuildId) {
  return Err<BuildProgress>(StatusCode::NotFound, kComponent, "no such build");
}

Result<PanoramaRef> PanoramaBuildManager::Panorama(BuildId) {
  return Err<PanoramaRef>(StatusCode::NotFound, kComponent, "no such build");
}

Result<GhostReport> PanoramaBuildManager::Ghosts(BuildId) {
  return Err<GhostReport>(StatusCode::NotFound, kComponent, "no such build");
}

Status PanoramaBuildManager::Invalidate(BuildId, std::span<const NodeId>) {
  return Fail(StatusCode::NotFound, kComponent, "no such build");
}

Status PanoramaBuildManager::Cancel(BuildId) {
  return Fail(StatusCode::NotFound, kComponent, "no such build");
}

}  // namespace sphanorama
