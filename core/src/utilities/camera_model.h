#pragma once

#include "sphanorama/types.h"

namespace sphanorama {

// The lens, as maths. Projection between a direction the camera is looking in and the pixel that
// direction lands on, and back.
//
// This is the first code in the repository that reads `Intrinsics` as *optics*. The struct has been
// threaded through `ICoveragePlannerEngine::Plan` and `IRegistrationEngine::Refine` since the
// architecture was written and never opened for what it describes: `CaptureSessionManager` reads
// `width` and `height` to write into a session document — a frame size, which is what those two
// are — and leaves the focal length, the optical centre and all five distortion coefficients at
// zero, because a phone lens is *estimated during a build* (types.h) and there has been no build. Everything Phase 2 does — rendering a synthetic
// frame, measuring a match residual in pixels, refining a rotation, projecting to
// equirectangular — is this transform or its inverse, so it comes first and everything else
// consumes it.
//
// **Directions, not points.** A panorama built from a phone held at one place is a pure rotation
// (ADR 0005's whole premise), so the model has no notion of depth or translation: a direction in
// camera space is all there is to project, and every point along it lands on the same pixel.
//
// **Camera space** is the convention the rest of the core already uses: **-Z forward, +Y up, +X
// right**, matching `Direction(q)` in utilities/quaternion.h and the browser's
// DeviceOrientation. **Image space** is the ordinary raster one: **+x right, +y down**, origin at
// the top-left corner of the image, so a pixel centre sits at a half-integer. The Y axis flips
// between the two, and that flip is the single easiest thing here to get silently wrong — it is
// pinned by a test that a direction pointing up lands *above* the optical centre.
//
// **Distortion is Brown-Conrady** in OpenCV's parameter convention, so `k1 k2 p1 p2 k3` from a
// calibration elsewhere can be dropped into `Intrinsics` unchanged — those live in normalised
// coordinates and do not care where the origin is.
//
// `cx` and `cy` from such a calibration are a different matter and are **half a pixel out**. OpenCV
// puts a pixel's centre at an integer, so a centred lens has `cx = (width - 1) / 2`; the corner
// origin used here puts it at `width / 2`. The half pixel is nothing for a field of view and is
// not nothing for a residual measured in pixels, which is what registration will do with it.

// A point in image space. Not `Vec3` with a dead component, and not a contract type: nothing here
// crosses the boundary, and a two-component pixel that cannot be mistaken for a direction is
// worth more than the reuse.
struct Pixel {
  double x = 0, y = 0;
};

// Whether these intrinsics describe a camera at all: every field this model reads finite, a
// positive focal length on both axes, and an image with area.
//
// A default-constructed `Intrinsics` is *not* usable, and that is the point rather than an
// oversight — it is what the capture session holds today, and nothing downstream should be able
// to start quietly trusting it without this answering false first.
bool IsUsableLens(const Intrinsics& lens);

// Intrinsics for a lens of a stated field of view over an image of a stated size, with no
// distortion.
//
// This is the only place intrinsics are *invented* rather than measured, and it is deliberately
// narrow: it exists to seed an estimate and to render synthetic datasets whose ground truth is
// known because the lens was chosen. A real lens is estimated by `IRegistrationEngine::Refine`
// and arrives with `estimated` set; this does not set it.
//
// Returns an unusable lens rather than a plausible one for a field of view that is not a lens —
// zero, negative, 180 degrees or wider, or not a number. `IsUsableLens` is how a caller asks.
Intrinsics LensFromFieldOfView(double horizontalFovDeg, double verticalFovDeg, int32_t width,
                               int32_t height);

// The field of view these intrinsics imply, in degrees: the angle from the optical centre out to
// one edge plus the angle out to the other, measured by asking the model where those edges look —
// so distortion counts, and a barrelled lens reports the wider angle it genuinely sees.
//
// A lens whose optical centre is off sees *less than a centred one of the same focal length*. The
// two angles' tangents sum to width/fx however the centre moves, so the sum is constrained; atan is
// concave, so a constrained sum of two of them is largest when they are equal, which is the centred
// case. Note which comparison that is: against the centred lens, not against twice the wider half.
// The weaker claim needs only monotonicity, and an earlier version of this comment proved it by
// accident while stating the stronger one.
//
// **Nothing consumes this yet.** `RingsCoveragePlannerEngine::Plan` takes `Intrinsics` unnamed and
// tessellates from `CapturePlanSpec`'s angles, which the page fills in from what the browser
// reports. Wiring the planner to a measured lens is Phase 2 work and is not done here; the
// definition is written the way the planner will need it — how much of the sphere a frame actually
// covers — so that the day it is wired the answer does not quietly change.
//
// Answers 0 for an unusable lens, which is the contract's word for "will not say" (types.h) and
// the same silence `RingsCoveragePlannerEngine::Plan` already refuses to tessellate.
double HorizontalFovDeg(const Intrinsics& lens);
double VerticalFovDeg(const Intrinsics& lens);

// Named for the answer rather than the act, because `Projection` is taken: in types.h it is the
// *output* projection a panorama is rendered into, Equirectangular or Cubemap. Two unrelated
// meanings of one word, and the contract had it first.
struct ProjectedPixel {
  // Only when `valid`. A refusal leaves this at the origin, which under the corner convention above
  // is the image's top-left corner — an ordinary in-bounds pixel — so there is no reading of this
  // field that detects a refusal. The flag is the answer.
  Pixel pixel;
  // Whether the direction has an image at all. Three different states answer false, and none of
  // them may be a pixel: an unusable lens or a direction that is not a measurement; a direction
  // at or behind the plane through the optical centre, which no forward-facing lens sees; and a
  // radius at or beyond the *first* point where the distortion polynomial stops increasing, past
  // which the model folds the image back over itself and two directions share a pixel. First,
  // rather than nearest: the polynomial can turn negative and come back, and a radius on the far
  // side of that dip is folded over however healthy the slope looks where it lands.
  bool valid = false;
};

// A direction in camera space projected to the pixel it lands on. The direction need not be a
// unit vector; only its bearing is read.
ProjectedPixel Project(const Intrinsics& lens, const Vec3& cameraSpace);

struct UnprojectedDirection {
  // Unit, and only when `valid`. A refusal leaves this zero, which is not a direction at all —
  // `IsUsableVector` says it is finite and `Normalize` answers it with zero again, so a caller that
  // skips the flag gets something that propagates quietly rather than failing. The flag is the
  // answer; this is only the payload.
  Vec3 direction;
  bool valid = false;
};

// The direction that lands on a pixel — `Project` run backwards.
//
// Brown-Conrady has no closed-form inverse, so this iterates, and it **refuses rather than
// answering approximately** when the iteration does not land back on the pixel it was given. A
// distortion strong enough not to invert is a lens this model cannot describe, and a best guess
// there is a wrong rotation later that nothing would attribute to the lens.
UnprojectedDirection Unproject(const Intrinsics& lens, const Pixel& pixel);

}  // namespace sphanorama
