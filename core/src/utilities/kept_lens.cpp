#include "utilities/kept_lens.h"

#include <algorithm>
#include <cmath>

#include "utilities/camera_model.h"

namespace sphanorama {
namespace {

constexpr const char* kComponent = "KeptLens";

bool IsFigure(double value) { return std::isfinite(value) && value >= 0.0; }

// The weight on the kept lens, of one, that minimises the combined uncertainty
//   w^2 keptNoise^2 + (1-w)^2 noise^2 + (w keptModel + (1-w) model)^2.
// The model errors add rather than combine as independent errors: both are the one lens misread,
// and `FocalScaleShift` reports each as a size without its sign, so they are taken to lean the same
// way. Where they are equal this is the inverse-variance weight by the noise. The four figures are
// scaled by the largest first, since the weight depends only on their ratios and their squares
// leave the doubles below 1e-154 and above 1e154.
double KeptWeight(double keptNoise, double keptModel, double noise, double model) {
  const double scale = std::max({keptNoise, keptModel, noise, model});
  if (scale == 0.0) return 1.0;
  keptNoise /= scale;
  keptModel /= scale;
  noise /= scale;
  model /= scale;
  const double apart = keptModel - model;
  const double denominator = keptNoise * keptNoise + noise * noise + apart * apart;
  // Zero only where neither has noise and their model errors agree, so that every weight is as sure
  // as every other; the kept lens is not moved.
  if (denominator == 0.0) return 1.0;
  return std::clamp((noise * noise - model * apart) / denominator, 0.0, 1.0);
}

}  // namespace

Result<KeptLens> AmendKeptLens(const KeptLens& kept, const GlobalSolution& capture) {
  if (kept.captures < 0) {
    return Err<KeptLens>(StatusCode::InvalidArgument, kComponent, "a negative count of captures");
  }
  if (kept.captures > 0 &&
      (!IsUsableLens(kept.lens) || !IsFigure(kept.noise) || !IsFigure(kept.modelError))) {
    return Err<KeptLens>(StatusCode::InvalidArgument, kComponent,
                         "the kept lens cannot project, or its noise or model error is not a figure");
  }
  if (!IsFigure(capture.focalScale)) {
    return Err<KeptLens>(StatusCode::InvalidArgument, kComponent, "the capture's scale is not a figure");
  }
  if (capture.focalScale == 0.0) return Ok(kept);
  if (!IsUsableLens(capture.intrinsics) || !IsFigure(capture.focalSpread) ||
      !IsFigure(capture.focalModelError)) {
    return Err<KeptLens>(StatusCode::InvalidArgument, kComponent,
                         "the capture's lens cannot project, or its spread or model error is not a figure");
  }

  Intrinsics measured = capture.intrinsics;
  measured.fx *= capture.focalScale;
  measured.fy *= capture.focalScale;
  if (kept.captures == 0) {
    KeptLens first;
    first.lens = measured;
    first.noise = capture.focalSpread;
    first.modelError = capture.focalModelError;
    first.lens.focalUncertainty = std::hypot(first.noise, first.modelError);
    first.lens.estimated = true;
    first.captures = 1;
    return Ok(first);
  }
  // Products of two positive int32s, which an int64 holds.
  if (static_cast<int64_t>(measured.width) * kept.lens.height !=
      static_cast<int64_t>(measured.height) * kept.lens.width) {
    return Err<KeptLens>(StatusCode::InvalidArgument, kComponent,
                         "the capture's frame is another shape than the kept lens's");
  }

  const double w = KeptWeight(kept.noise, kept.modelError, capture.focalSpread, capture.focalModelError);
  const double keptEdge = std::max(kept.lens.width, kept.lens.height);
  const double captureEdge = std::max(measured.width, measured.height);
  const double captureFx = measured.fx * keptEdge / captureEdge;

  KeptLens amended = kept;
  // At the ends the weight is exact, and so is the lens it picks.
  amended.lens.fx = w == 1.0   ? kept.lens.fx
                    : w == 0.0 ? captureFx
                               : std::exp(w * std::log(kept.lens.fx) + (1.0 - w) * std::log(captureFx));
  amended.lens.fy = kept.lens.fy * amended.lens.fx / kept.lens.fx;
  amended.noise = std::hypot(w * kept.noise, (1.0 - w) * capture.focalSpread);
  amended.modelError = w * kept.modelError + (1.0 - w) * capture.focalModelError;
  amended.lens.focalUncertainty = std::hypot(amended.noise, amended.modelError);
  amended.lens.estimated = true;
  if (amended.captures < std::numeric_limits<int32_t>::max()) ++amended.captures;
  return Ok(amended);
}

}  // namespace sphanorama
