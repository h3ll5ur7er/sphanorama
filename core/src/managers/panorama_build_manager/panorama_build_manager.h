#pragma once

#include "sphanorama/managers/panorama_build_manager.h"

namespace sphanorama {

// Owns a build. Every call reports Unsupported today because the engines it sequences are null
// objects that refuse honestly (V7, V8) — a build manager that returned an empty panorama would
// make a broken pipeline look like a working one.
//
// It exists now rather than later so the facade exposes the real interface, and so the shape of
// the incremental rebuild (ADR 0004) is fixed before anything depends on it.
//
// It takes no engines yet on purpose: it has no build to run them over, and a constructor
// holding references nothing uses is a dependency claimed rather than needed. They arrive with
// Start.
class PanoramaBuildManager final : public IPanoramaBuildManager {
 public:

  Result<BuildId> Start(SessionId session, const BuildSpec& spec) override;
  Result<BuildProgress> Poll(BuildId build) override;
  Result<PanoramaRef> Panorama(BuildId build) override;
  Result<GhostReport> Ghosts(BuildId build) override;
  Status Invalidate(BuildId build, std::span<const NodeId> dirty) override;
  Status Cancel(BuildId build) override;
};

}  // namespace sphanorama
