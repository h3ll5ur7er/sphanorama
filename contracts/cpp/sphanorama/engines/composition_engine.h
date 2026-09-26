#pragma once
#include <span>
#include "sphanorama/types.h"

namespace sphanorama {

// V8 — how pixels become one image.
//
// **The methods that read pixels are handed the frames**, `frames[i]` being the frame of
// `solution.frames[i]`: a `GlobalSolution` names its frames by `FrameId`, and an engine cannot read
// a pixel through an id — only through the `FrameRef` the store gave out (ADR 0068). The frames stay
// the caller's; reading them pins them, and pinning a spilled frame faults it back into the heap. `CompensateExposure` had them from the start and the others had not, which is the gap
// `EstimatePairwise` had with its lens (ADR 0054) found again before an implementation rather than
// by one.
class ICompositionEngine {
 public:
  virtual ~ICompositionEngine() = default;

  virtual Result<GainMap> CompensateExposure(const GlobalSolution&,
                                             std::span<const FrameRef>) = 0;

  // Disagreement between a cell's own candidates localises movers directly (docs/04 §4.5): the
  // burst we kept for selection is also the ghost detector.
  virtual Result<GhostReport> DetectGhosts(const GlobalSolution&,
                                           std::span<const Candidate> allCandidates) = 0;

  virtual Result<SeamMap> FindSeams(const GlobalSolution&, std::span<const FrameRef> frames,
                                    const GainMap&, const GhostReport&, const BuildSpec&) = 0;

  // Tiled so that a retake re-blends a handful of tiles rather than the whole sphere.
  virtual Result<FrameRef> BlendTile(const GlobalSolution&, std::span<const FrameRef> frames,
                                     const GainMap&, const SeamMap&, const BuildSpec&,
                                     int32_t tileX, int32_t tileY) = 0;

  // The whole sphere, small and at once, for a person to look at while the tiles are made — and
  // for a test to look at, since a rotation a degree out shows here as a step where two frames meet.
  //
  // An equirectangular `RGBA8` frame twice as wide as it is high, the largest such no wider than
  // `maxWidth`, in the convention `utilities/equirect.h` states. Alpha is coverage: 255 where some
  // frame sees the direction and 0, with black, where none does — a gap in a capture is not a
  // colour. **It belongs to the caller**, who must `Forget` it.
  //
  // `gains` may be empty, meaning none; otherwise it names the solution's frames in the solution's
  // order, each gain finite and above zero, and multiplies that frame's colour.
  //
  // Each frame is read on its own and put back in the tier it was found in, so the preview of a
  // sphere larger than the heap can still be made: what it holds at once is one frame and the
  // answer.
  //
  // `InvalidArgument` for frames that are not the solution's in its order, a frame another size than
  // the solution's lens, a handle claiming more bytes than the store holds for it, a lens that
  // cannot project when there is a frame to project, a rotation that is not one, gains that do not
  // name the frames or are not figures, 65,535 frames or more, and a `maxWidth` under 2; `Unsupported` for a frame that is not `RGBA8`; `Internal` for an answer the store describes
  // as another shape than was asked; the store's own status when a frame cannot be pinned or
  // released, or the answer allocated. A refusal gives the answer back, and says so when the store
  // will not take it.
  virtual Result<FrameRef> RenderPreview(const GlobalSolution&, std::span<const FrameRef> frames,
                                         const GainMap&, int32_t maxWidth) = 0;
};

}  // namespace sphanorama
