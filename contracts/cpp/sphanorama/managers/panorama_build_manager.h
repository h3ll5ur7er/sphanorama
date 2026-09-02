#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V2 — how a panorama is built, including incremental rebuild.
// @boundary
class IPanoramaBuildManager {
 public:
  virtual ~IPanoramaBuildManager() = default;

  virtual Result<BuildId> Start(SessionId session, const BuildSpec& spec) = 0;
  virtual Result<BuildProgress> Poll(BuildId build) = 0;
  virtual Result<PanoramaRef> Panorama(BuildId build) = 0;
  virtual Result<GhostReport> Ghosts(BuildId build) = 0;

  // The mechanism behind both retakes and manual candidate switching: recompute only the
  // transitive closure downstream of the changed cells (docs/04 §4.4). An incremental rebuild
  // must equal a full rebuild bit for bit — that invariant is the safety net under the feature.
  virtual Status Invalidate(BuildId build, std::span<const NodeId> dirty) = 0;

  virtual Status Cancel(BuildId build) = 0;
};

}  // namespace sphanorama
