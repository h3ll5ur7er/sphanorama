#pragma once
#include <cstdint>
#include "sphanorama/types.h"

namespace sphanorama {

// The longest edge a preview may be asked for.
//
// A budget rather than a taste. RGBA is four bytes a pixel and nothing in this path compresses
// anything, so the cap is what decides how much a strip costs: at 256 a 4:3 preview is 192 KB and
// a cell's eight candidates are 1.5 MB, against a browser heap ceiling of 128 MB (ADR 0023). The
// unreduced frames would be 39 MB. Asking for more is refused rather than clamped — a caller that
// wanted a full frame is asking for the thing the reduction exists to avoid, and quietly handing
// back something else is how a budget stops being one.
constexpr int32_t kFramePreviewMaxEdge = 256;

// V16 — how a frame the core is holding is made small enough to look at.
//
// It exists because reading a stored frame *out* had no owner. `IFrameStoreAccess` says where
// pixel bytes live (V11) and `Pin` is the only route to them, but a pin reaches no further than
// the core; `IFrameQualityEngine` says what "best" means (V6) and a thumbnail is not a judgement;
// `ICompositionEngine` says how many frames become one image (V8) and this is one frame becoming
// a smaller one. What varies here is the reduction itself — its factor, its filter, and the pixel
// format it lands in — and it varies with the surface doing the reviewing and with what the
// crossing costs, neither of which any of those three can see (ADR 0038).
//
// Stateless, like every engine: a preview is a pure function of a frame and a size.
class IFramePreviewEngine {
 public:
  virtual ~IFramePreviewEngine() = default;

  // A reduced copy of `frame`, no longer than `maxEdge` on its long edge, in RGBA8.
  //
  // The aspect ratio is kept, so `maxEdge` bounds the long edge and the short one follows. A
  // frame already inside the bound comes back at its own size rather than being enlarged: this
  // reduces, and inventing pixels is a different operation with a different name.
  //
  // Fallible for the reasons reading any frame is: the frame may be gone, its bytes may be in a
  // tier that will not give them back, and its format may be one nothing here can read. Whatever
  // happens, the frame is released — a preview that left a pin behind would hold the review
  // client's frames in the heap for the life of the session.
  virtual Result<FramePreview> Reduce(const FrameRef& frame, int32_t maxEdge) = 0;
};

}  // namespace sphanorama
