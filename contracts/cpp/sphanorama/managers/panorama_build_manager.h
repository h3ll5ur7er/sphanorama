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
  // the cell's manual pick where one was recorded and the cell still holds it, and otherwise the
  // best-ranked frame the capture wrote down whose pose was measured — and the capture's field of
  // view at the frames' size (ADR 0070). Frames are paired only where their poses were measured,
  // so a cell whose frame was not — a pick of one, or a cell with no other — is left out of the
  // panorama, and the build still completes. What it reads is what the capture last managed to
  // write down: a rewrite that failed leaves the document behind the capture, and a build started
  // meanwhile uses the older ranking — refused only where that would override a pick.
  //
  // Only the checks happen here. The work is done by `Poll`, one step a call — a frame's features,
  // a pair, the solve, the preview — so that no call holds the core's one thread for a whole build.
  //
  // `NotFound` for a project that does not exist or holds no capture; `Unsupported` for a document
  // this build cannot read, as `ICaptureSessionManager::Resume` answers; `FailedPrecondition` while
  // another build is running, on a store with no spill tier, for a capture with nothing in it, one
  // whose frames the store no longer holds — a tab reloaded without resuming, or a tier a newer
  // capture emptied, or a document a failed rewrite left naming frames a retake had discarded —
  // one naming a frame in two cells, one with no measured pose, frames of more than one size, a
  // field of view that is not a lens, and a recorded pick newer than the document, which a failed
  // rewrite also leaves; `Internal` for a recorded pick that is not a candidate at
  // all; the store's own status, whole, where it could not read the title, the document or a pick,
  // say which tier it holds, or say where a frame is — none of which says the thing is absent;
  // `Unsupported` for a cubemap and `InvalidArgument` for an output narrower than two. Starting
  // after a build has finished releases the finished one's panorama, and if the store will not,
  // this refuses with the store's status and the finished build stands.
  virtual Result<BuildId> Start(ProjectId project, const BuildSpec& spec) = 0;

  // Does the build's next step and says where it has got to. A step that fails puts back any
  // capture frame it left out of its tier, and what the store will not put back yet is retried by
  // `Cancel` and the next `Start`, as the build's own frames are. Putting back only ever cools a
  // frame and releases only the pin an engine left, once, so a retry leaves a frame something else
  // has moved or pinned since where that component put it. The preview is not drawn when a frame's
  // tier cannot be read, since that is what a refusal would need to put it back.
  // A finished build — `Complete` or
  // `Failed` — answers the same progress however often it is asked, and `failure` says why one
  // failed. `fraction` counts steps: it never goes back, and it is one exactly when the build is
  // complete.
  virtual Result<BuildProgress> Poll(BuildId build) = 0;

  // A complete build's panorama: today only its preview, an equirectangular `RGBA8` frame no wider
  // than the spec's `outputWidth` or 2048, and no tiles. **The core holds it**, until the build is
  // cancelled or another is started, so a caller reads it and does not `Forget` it.
  // `FailedPrecondition` for a build that has not completed, and for one whose panorama the store
  // no longer holds — a new capture empties the store it is in; the store's own status where it
  // could not say.
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
  // release or forget is kept, the store's status is answered, and cancelling again retries it; one
  // the store no longer holds at all is given back already.
  virtual Status Cancel(BuildId build) = 0;
};

}  // namespace sphanorama
