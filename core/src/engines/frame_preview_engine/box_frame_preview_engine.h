#pragma once

#include <cstdint>

#include "sphanorama/engines/frame_preview_engine.h"
#include "sphanorama/resource_access/frame_store_access.h"

namespace sphanorama {

// V16 — box-averaged into whole blocks, which is the smallest thing that produces an honest
// picture.
//
// Integer block sizes rather than interpolation, for the same reason the sharpness measure
// downscales that way: a resampler is a second thing to get wrong, and what a strip needs is a
// recognisable frame rather than a beautiful one. Averaging also happens to be the right filter
// for the job — taking every tenth pixel instead would alias a textured scene into moiré and make
// two frames of the same wall look different from each other.
//
// It reads pixels, so it holds `IFrameStoreAccess`. That is one of the two resource accesses an
// engine may touch (docs/03 §3.3 rule 5): pixel residency is a property of the device rather than
// of the algorithm.
//
// What it does not do, and why, so a zero is never mistaken for a measurement: `NV12` and `I420`
// are refused rather than reduced. Their luma plane is readable — the sharpness engine reads it —
// but a preview is a colour image, and turning one of those into RGB means a chroma upsample and
// a colour matrix that would be invented here rather than measured. A grey preview of a colour
// frame is a wrong answer that looks like a right one.
class BoxFramePreviewEngine final : public IFramePreviewEngine {
 public:
  explicit BoxFramePreviewEngine(IFrameStoreAccess& frames) : frames_(frames) {}

  Result<FramePreview> Reduce(const FrameRef& frame, int32_t maxEdge) override;

 private:
  IFrameStoreAccess& frames_;
};

}  // namespace sphanorama
