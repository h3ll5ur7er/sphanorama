#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V2 — how a panorama is built, including incremental rebuild.
// @boundary @facade
class IPanoramaBuildManager {
 public:
  virtual ~IPanoramaBuildManager() = default;

  // Builds from what the project's capture wrote down: its session document, one frame per cell —
  // the cell's manual pick where one was recorded, and the ranking's best otherwise — and the
  // capture's field of view at the frames' size (ADR 0070).
  //
  // Only the checks happen here. The work is done by `Poll`, one step a call — a frame's features,
  // a pair, the solve, the preview — so that no call holds the core's one thread for a whole build.
  //
  // `NotFound` for a project that does not exist or holds no capture; `Unsupported` for a document
  // this build cannot read, as `ICaptureSessionManager::Resume` answers; `FailedPrecondition` while
  // another build is running, for a capture with nothing in it, one whose frames the store no
  // longer holds — a tab reloaded without resuming, or a tier a newer capture emptied — frames of
  // more than one size, a field of view that is not a lens, and a recorded pick naming a candidate
  // its cell does not hold; `Internal` for a recorded pick that is not a candidate at all;
  // `Unsupported` for a cubemap and `InvalidArgument` for an output narrower than two. Starting
  // after a build has finished releases the finished one's panorama, and if the store will not,
  // this refuses with the store's status and the finished build stands.
  virtual Result<BuildId> Start(ProjectId project, const BuildSpec& spec) = 0;

  // Does the build's next step and says where it has got to. A finished build — `Complete` or
  // `Failed` — answers the same progress however often it is asked, and `failure` says why one
  // failed. `fraction` counts steps: it never goes back, and it is one exactly when the build is
  // complete.
  virtual Result<BuildProgress> Poll(BuildId build) = 0;

  // A complete build's panorama: today only its preview, an equirectangular `RGBA8` frame no wider
  // than the spec's `outputWidth` or 2048, and no tiles. **The core holds it**, until the build is
  // cancelled or another is started, so a caller reads it and does not `Forget` it.
  // `FailedPrecondition` for a build that has not completed.
  virtual Result<PanoramaRef> Panorama(BuildId build) = 0;

  // `Unsupported` for a build that exists, because nothing detects movers yet — an empty report
  // would read as a scene with none.
  virtual Result<GhostReport> Ghosts(BuildId build) = 0;

  // The mechanism behind both retakes and manual candidate switching: recompute only the
  // transitive closure downstream of the changed cells (docs/04 §4.4). An incremental rebuild
  // must equal a full rebuild bit for bit — that invariant is the safety net under the feature.
  //
  // `Unsupported` for a build that exists, until that rebuild does: a success here would be a
  // panorama claiming a retake it never saw. Starting a new build is the whole rebuild meanwhile.
  virtual Status Invalidate(BuildId build, std::span<const NodeId> dirty) = 0;

  // Ends a build, running or finished, and gives back everything it holds — the features it had
  // read and the panorama it made. After it the build is `NotFound`. A frame the store will not
  // forget is kept, the store's status is answered, and cancelling again retries it.
  virtual Status Cancel(BuildId build) = 0;
};

}  // namespace sphanorama
