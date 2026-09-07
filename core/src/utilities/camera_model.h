#pragma once

#include "sphanorama/types.h"

namespace sphanorama {

// The lens, as maths. Projection between a direction the camera is looking in and the pixel that
// direction lands on, and back.
//
// This is the first code in the repository that reads a field of `Intrinsics`. Until now the
// struct was threaded through `ICoveragePlannerEngine::Plan` and `IRegistrationEngine::Refine`
// and never opened: `CaptureSessionManager` fills in `width` and `height` and leaves the focal
// length and all five distortion coefficients at zero, because a phone lens is *estimated during
// a build* (types.h) and there has been no build. Everything Phase 2 does — rendering a synthetic
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
// calibration elsewhere can be dropped into `Intrinsics` unchanged.

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
// one edge plus the angle out to the other. A lens whose optical centre is not in the middle of
// its image therefore sees *less* than twice its wider half — the two halves are unequal, and the
// sum of two atans is largest when they are equal. This is the number the coverage planner
// tessellates from, so it has to be how much of the sphere a frame covers rather than what a
// centred lens of this focal length would have covered.
//
// Answers 0 for an unusable lens, which is the contract's word for "will not say" (types.h) and
// the same silence `RingsCoveragePlannerEngine::Plan` already refuses to tessellate.
double HorizontalFovDeg(const Intrinsics& lens);
double VerticalFovDeg(const Intrinsics& lens);

// Named for the answer rather than the act, because `Projection` is taken: in types.h it is the
// *output* projection a panorama is rendered into, Equirectangular or Cubemap. Two unrelated
// meanings of one word, and the contract had it first.
struct ProjectedPixel {
  Pixel pixel;
  // Whether the direction has an image at all. Three different states answer false, and none of
  // them may be a pixel: an unusable lens or a direction that is not a measurement; a direction
  // at or behind the plane through the optical centre, which no forward-facing lens sees; and a
  // radius past the point where the distortion polynomial stops increasing, beyond which the
  // model folds the image back over itself and two directions share a pixel.
  bool valid = false;
};

// A direction in camera space projected to the pixel it lands on. The direction need not be a
// unit vector; only its bearing is read.
ProjectedPixel Project(const Intrinsics& lens, const Vec3& cameraSpace);

struct UnprojectedDirection {
  Vec3 direction;   // unit
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
