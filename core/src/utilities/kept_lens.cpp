#include "utilities/kept_lens.h"

#include <algorithm>
#include <cmath>

#include "utilities/camera_model.h"

namespace sphanorama {
namespace {

constexpr const char* kComponent = "KeptLens";

bool IsFigure(double value) { return std::isfinite(value) && value >= 0.0; }

// And the two together: finite figures past about 1e308 combine to an infinity, which is a guess.
bool AreFigures(double noise, double modelError) {
  return IsFigure(noise) && IsFigure(modelError) && std::isfinite(std::hypot(noise, modelError));
}

bool SameShape(int32_t width, int32_t height, const Intrinsics& lens) {
  // Products of two positive int32s, which an int64 holds.
  return static_cast<int64_t>(width) * lens.height == static_cast<int64_t>(height) * lens.width;
}

Status CheckKept(const KeptLens& kept) {
  if (kept.captures < 0) {
    return Fail(StatusCode::InvalidArgument, kComponent, "a negative count of captures");
  }
  if (kept.captures > 0 && (!IsUsableLens(kept.lens) || !AreFigures(kept.noise, kept.modelError))) {
    return Fail(StatusCode::InvalidArgument, kComponent,
                "the kept lens cannot project, or its noise and model error are not figures");
  }
  return Status::Ok();
}

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

// Every kept lens this answers with: the copies the figures imply written from them, whatever the
// kept lens came in with, and a lens that cannot project refused rather than kept, since no later
// call could read it to replace it.
Result<KeptLens> Settled(KeptLens kept) {
  if (kept.captures == 0) return Ok(kept);
  if (!IsUsableLens(kept.lens)) {
    return Err<KeptLens>(StatusCode::InvalidArgument, kComponent,
                         "the lens the capture would leave kept cannot project");
  }
  kept.lens.focalUncertainty = std::hypot(kept.noise, kept.modelError);
  kept.lens.estimated = true;
  return Ok(kept);
}

}  // namespace

Result<KeptLens> AmendKeptLens(const KeptLens& kept, const GlobalSolution& capture) {
  if (Status checked = CheckKept(kept); !checked.ok()) return checked;
  if (!IsFigure(capture.focalScale)) {
    return Err<KeptLens>(StatusCode::InvalidArgument, kComponent, "the capture's scale is not a figure");
  }
  if (capture.focalScale == 0.0) return Settled(kept);
  if (!IsUsableLens(capture.intrinsics) || !AreFigures(capture.focalSpread, capture.focalModelError)) {
    return Err<KeptLens>(StatusCode::InvalidArgument, kComponent,
                         "the capture's lens cannot project, or its spread and model error are not figures");
  }

  Intrinsics measured = capture.intrinsics;
  measured.fx *= capture.focalScale;
  measured.fy *= capture.focalScale;
  if (kept.captures == 0) {
    KeptLens first;
    first.lens = measured;
    first.noise = capture.focalSpread;
    first.modelError = capture.focalModelError;
    first.captures = 1;
    return Settled(first);
  }
  if (!SameShape(measured.width, measured.height, kept.lens)) {
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
  if (amended.captures < std::numeric_limits<int32_t>::max()) ++amended.captures;
  return Settled(amended);
}

Result<Intrinsics> KeptLensFor(const KeptLens& kept, int32_t width, int32_t height) {
  if (width <= 0 || height <= 0) {
    return Err<Intrinsics>(StatusCode::InvalidArgument, kComponent, "the frame has no size");
  }
  if (Status checked = CheckKept(kept); !checked.ok()) return checked;
  if (kept.captures == 0) {
    return Err<Intrinsics>(StatusCode::NotFound, kComponent, "no lens is kept");
  }
  if (!SameShape(width, height, kept.lens)) {
    return Err<Intrinsics>(StatusCode::InvalidArgument, kComponent,
                           "the frame is another shape than the kept lens's");
  }
  const double ratio = static_cast<double>(std::max(width, height)) /
                       static_cast<double>(std::max(kept.lens.width, kept.lens.height));
  Intrinsics lens = kept.lens;
  lens.fx *= ratio;
  lens.fy *= ratio;
  lens.cx *= ratio;
  lens.cy *= ratio;
  lens.width = width;
  lens.height = height;
  lens.focalUncertainty = std::hypot(kept.noise, kept.modelError);
  lens.estimated = true;
  // Scaling can carry a usable lens out of use: a focal length past the doubles, or a principal
  // point that rounds onto the edge of the larger frame.
  if (!IsUsableLens(lens)) {
    return Err<Intrinsics>(StatusCode::InvalidArgument, kComponent,
                           "the kept lens cannot project at this size");
  }
  return Ok(lens);
}

}  // namespace sphanorama
