#include "utilities/camera_model.h"

#include <cmath>

#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr double kDegToRad = 0.017453292519943295;
constexpr double kRadToDeg = 57.29577951308232;

// How far an unprojected direction may land from the pixel it came from when projected back, in
// normalised image units. At the focal lengths a phone has this is under a millionth of a pixel —
// tight enough that a distortion which does not invert is caught, loose enough that an ordinary
// one is never refused for rounding.
constexpr double kInverseToleranceNormalised = 1e-9;

// Brown-Conrady inverts by fixed-point iteration, which stops as soon as it has stopped moving —
// an ordinary phone lens settles in about five passes, so the ceiling below is not what the common
// case costs.
//
// The ceiling is five hundred because of what happens near the fold. There the iteration still
// converges, but linearly with a ratio approaching 1, so it crawls. Measured on k1 = -0.9: a
// budget of 20 accepts pixels out to 89.6% of the radius that actually has a preimage, 100 reaches
// 99.6%, and 500 reaches 99.98% — the missing sliver is the fold itself, where the convergence
// ratio goes to 1 and no finite budget arrives, and where the inverse is ill-conditioned anyway.
// A smaller budget does not refuse *approximately* — it refuses
// perfectly well-defined pixels near the edge of a wide lens, and calls it a lens it cannot
// describe.
constexpr int kInverseIterations = 500;

// The iteration has settled when a pass moves it less than this, in normalised image units. Three
// orders of magnitude tighter than the tolerance the answer is finally judged against, because a
// linearly converging sequence can still be that ratio away from its limit when a single step has
// become small — the early exit must not be what decides the answer is good enough.
constexpr double kSettledStepNormalised = 1e-14;

// The radial polynomial, and its derivative with respect to r rather than to r^2.
//
// The derivative is the one that decides whether a pixel exists at all. The map is
// r -> r * radial(r^2), and where that stops increasing the image folds back over itself: two
// directions land on one pixel and there is no inverse to find. d/dr of r * (1 + k1 r^2 +
// k2 r^4 + k3 r^6) is 1 + 3 k1 r^2 + 5 k2 r^4 + 7 k3 r^6.
double Radial(const Intrinsics& lens, double r2) {
  return 1.0 + r2 * (lens.k1 + r2 * (lens.k2 + r2 * lens.k3));
}

double RadialSlope(const Intrinsics& lens, double r2) {
  return 1.0 + r2 * (3.0 * lens.k1 + r2 * (5.0 * lens.k2 + r2 * 7.0 * lens.k3));
}

}  // namespace

bool IsUsableLens(const Intrinsics& lens) {
  // rollingShutterLineTimeNs is deliberately not read: it says when a row was exposed, not where a
  // direction lands, and a lens with an unknown line time is still a lens.
  if (!std::isfinite(lens.fx) || !std::isfinite(lens.fy) || !std::isfinite(lens.cx) ||
      !std::isfinite(lens.cy) || !std::isfinite(lens.k1) || !std::isfinite(lens.k2) ||
      !std::isfinite(lens.k3) || !std::isfinite(lens.p1) || !std::isfinite(lens.p2)) {
    return false;
  }
  if (lens.fx <= 0.0 || lens.fy <= 0.0) return false;
  return lens.width > 0 && lens.height > 0;
}

Intrinsics LensFromFieldOfView(double horizontalFovDeg, double verticalFovDeg, int32_t width,
                               int32_t height) {
  // A default Intrinsics is unusable, so every refusal below is a plain return: there is no way to
  // answer this wrongly and have the answer still look like a lens.
  Intrinsics lens;
  if (!std::isfinite(horizontalFovDeg) || !std::isfinite(verticalFovDeg)) return lens;
  if (horizontalFovDeg <= 0.0 || verticalFovDeg <= 0.0) return lens;
  // At 180 degrees the half-angle's tangent is infinite and a rectilinear lens has stopped
  // existing. RingsCoveragePlannerEngine refuses the same angle for the same reason.
  if (horizontalFovDeg >= 180.0 || verticalFovDeg >= 180.0) return lens;
  if (width <= 0 || height <= 0) return lens;

  const double halfWidth = static_cast<double>(width) / 2.0;
  const double halfHeight = static_cast<double>(height) / 2.0;
  lens.fx = halfWidth / std::tan(horizontalFovDeg * kDegToRad / 2.0);
  lens.fy = halfHeight / std::tan(verticalFovDeg * kDegToRad / 2.0);
  lens.cx = halfWidth;
  lens.cy = halfHeight;
  lens.width = width;
  lens.height = height;
  return lens;
}

// The angle from the optical centre out to each edge, summed. Not twice the angle to a half-width,
// which is the same number only while the optical centre is in the middle of the image and is 2.1
// degrees wrong on a 66-degree lens whose centre sits at 300 of 960 — the two half-angles are
// unequal, and because atan is concave their sum is *largest* when they are equal. A real
// calibration puts the centre a few pixels off and the difference is nothing; the definition still
// has to be the one the coverage planner needs, which is how much of the sphere a frame actually
// covers.
double HorizontalFovDeg(const Intrinsics& lens) {
  if (!IsUsableLens(lens)) return 0.0;
  const double toTheRight = std::atan((static_cast<double>(lens.width) - lens.cx) / lens.fx);
  return (std::atan(lens.cx / lens.fx) + toTheRight) * kRadToDeg;
}

double VerticalFovDeg(const Intrinsics& lens) {
  if (!IsUsableLens(lens)) return 0.0;
  const double toTheBottom = std::atan((static_cast<double>(lens.height) - lens.cy) / lens.fy);
  return (std::atan(lens.cy / lens.fy) + toTheBottom) * kRadToDeg;
}

ProjectedPixel Project(const Intrinsics& lens, const Vec3& cameraSpace) {
  ProjectedPixel out;
  if (!IsUsableLens(lens) || !IsUsableVector(cameraSpace)) return out;

  // -Z is forward, so depth is the negated Z. Zero or negative depth is the direction lying in or
  // behind the plane through the optical centre. Written as `> 0.0` rather than `<= 0.0` so that a
  // NaN reaching here refuses rather than passes.
  const double depth = -cameraSpace.z;
  if (!(depth > 0.0)) return out;

  const double xn = cameraSpace.x / depth;
  const double yn = -cameraSpace.y / depth;   // the world's up is the image's down
  if (!std::isfinite(xn) || !std::isfinite(yn)) return out;

  const double r2 = xn * xn + yn * yn;
  if (!(RadialSlope(lens, r2) > 0.0)) return out;
  const double radial = Radial(lens, r2);
  if (!(radial > 0.0)) return out;

  const double xd = xn * radial + 2.0 * lens.p1 * xn * yn + lens.p2 * (r2 + 2.0 * xn * xn);
  const double yd = yn * radial + lens.p1 * (r2 + 2.0 * yn * yn) + 2.0 * lens.p2 * xn * yn;

  const double u = lens.fx * xd + lens.cx;
  const double v = lens.fy * yd + lens.cy;
  if (!std::isfinite(u) || !std::isfinite(v)) return out;

  out.pixel = Pixel{u, v};
  out.valid = true;
  return out;
}

UnprojectedDirection Unproject(const Intrinsics& lens, const Pixel& pixel) {
  UnprojectedDirection out;
  if (!IsUsableLens(lens)) return out;
  if (!std::isfinite(pixel.x) || !std::isfinite(pixel.y)) return out;

  const double xd = (pixel.x - lens.cx) / lens.fx;
  const double yd = (pixel.y - lens.cy) / lens.fy;
  if (!std::isfinite(xd) || !std::isfinite(yd)) return out;

  double xn = xd;
  double yn = yd;
  for (int i = 0; i < kInverseIterations; ++i) {
    const double r2 = xn * xn + yn * yn;
    const double radial = Radial(lens, r2);
    if (!(radial > 0.0)) return out;
    const double tangentialX = 2.0 * lens.p1 * xn * yn + lens.p2 * (r2 + 2.0 * xn * xn);
    const double tangentialY = lens.p1 * (r2 + 2.0 * yn * yn) + 2.0 * lens.p2 * xn * yn;
    const double nextX = (xd - tangentialX) / radial;
    const double nextY = (yd - tangentialY) / radial;
    if (!std::isfinite(nextX) || !std::isfinite(nextY)) return out;
    const double stepX = nextX - xn;
    const double stepY = nextY - yn;
    xn = nextX;
    yn = nextY;
    if (stepX * stepX + stepY * stepY <= kSettledStepNormalised * kSettledStepNormalised) break;
  }

  const Vec3 direction = Normalize(Vec3{xn, -yn, -1.0});

  // Checked against the forward model, and by calling `Project` rather than by repeating its
  // arithmetic here. Two reasons: a fixed point that has stopped moving has not necessarily
  // stopped at the right place, and a pixel past the fold has to fall out of this check — which
  // is a property of `Project`, so asking `Project` is the only way the two agree on where the
  // fold is.
  const ProjectedPixel check = Project(lens, direction);
  if (!check.valid) return out;
  const double errorX = (check.pixel.x - pixel.x) / lens.fx;
  const double errorY = (check.pixel.y - pixel.y) / lens.fy;
  if (!(std::sqrt(errorX * errorX + errorY * errorY) <= kInverseToleranceNormalised)) return out;

  out.direction = direction;
  out.valid = true;
  return out;
}

}  // namespace sphanorama
