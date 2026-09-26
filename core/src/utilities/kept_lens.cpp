#include "utilities/kept_lens.h"

#include <algorithm>
#include <cmath>

namespace sphanorama {

KeptLens AmendKeptLens(const KeptLens& kept, const GlobalSolution& capture) {
  if (!capture.lensFitted) return kept;
  if (kept.captures == 0) {
    KeptLens first;
    first.lens = capture.intrinsics;
    first.noise = capture.focalSpread;
    first.modelError = capture.focalModelError;
    first.lens.focalUncertainty = std::hypot(first.noise, first.modelError);
    first.lens.estimated = true;
    first.captures = 1;
    return first;
  }
  if (kept.noise == 0.0) {
    KeptLens counted = kept;
    ++counted.captures;
    return counted;
  }
  const double keptEdge = std::max(kept.lens.width, kept.lens.height);
  const double captureEdge = std::max(capture.intrinsics.width, capture.intrinsics.height);
  const double captureLog = std::log(capture.intrinsics.fx * keptEdge / captureEdge);
  const double keptLog = std::log(kept.lens.fx);

  KeptLens amended = kept;
  if (capture.focalSpread == 0.0) {
    amended.noise = 0.0;
    amended.modelError = capture.focalModelError;
    amended.lens.fx = capture.intrinsics.fx * keptEdge / captureEdge;
  } else {
    const double keptWeight = 1.0 / (kept.noise * kept.noise);
    const double captureWeight = 1.0 / (capture.focalSpread * capture.focalSpread);
    const double total = keptWeight + captureWeight;
    amended.lens.fx = std::exp((keptWeight * keptLog + captureWeight * captureLog) / total);
    amended.noise = 1.0 / std::sqrt(total);
    // The bias of the weighted mean is the same weighting of each capture's own.
    amended.modelError =
        (keptWeight * kept.modelError + captureWeight * capture.focalModelError) / total;
  }
  amended.lens.fy = kept.lens.fy * amended.lens.fx / kept.lens.fx;
  amended.lens.focalUncertainty = std::hypot(amended.noise, amended.modelError);
  amended.lens.estimated = true;
  ++amended.captures;
  return amended;
}

}  // namespace sphanorama
