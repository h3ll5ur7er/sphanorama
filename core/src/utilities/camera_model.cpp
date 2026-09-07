#include "utilities/camera_model.h"

#include <cmath>

#include "utilities/quaternion.h"

namespace sphanorama {

// **On the layering of the guards below.** Almost every refusal in this file is individually
// removable without a test going red — measured: thirteen of the fourteen guards, the only
// exception being `Project`'s depth test. A reviewer found this and it is worth answering here
// rather than on a thread, because the answer is a design position and not an oversight.
//
// They are not redundant checks of the same question. Each one makes the *next* step's precondition
// locally true, and the reason that matters is what would otherwise be carrying the weight: NaN
// propagation through a polynomial. Delete `isfinite(xn)` and the refusal still happens — but only
// because `inf * 0` is NaN, NaN fails `> 0`, and the compiler did not assume otherwise. That is a
// guarantee `-ffast-math` withdraws, and one a different libm can round differently at the edges.
// A check that reads the value and refuses it does not depend on any of that.
//
// So the rule this file follows is: refuse at the earliest point the question is answerable, and do
// not rely on a later guard to catch what an earlier one can see. What that costs is exactly the
// property the reviewer measured — most guards have no test of their own, because the public API
// cannot reach them in isolation. What it buys is that the arithmetic is never asked to run on
// values nobody checked.
//
// The one place the layering is load-bearing rather than defensive is `Unproject`'s `check.valid`:
// a refused `ProjectedPixel` carries the pixel `(0, 0)`, so folding that test into the tolerance
// comparison below it would return a confident wrong direction for an input pixel of exactly
// `(0, 0)`, and for no other input.

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
// The ceiling is large because of what happens near a fold. There the iteration still converges,
// but linearly with a ratio approaching 1, so it crawls: on a lens with k1 = -0.9 and p2 = 0.6 a
// perfectly ordinary interior pixel needs 835 passes, and a budget of 500 refuses it — which is not
// an approximate answer but a well-defined pixel thrown away, the very thing this file claims not
// to do. That case was found by a reviewer, in a test of mine that had written the truncation down
// as if it were a property of the lens.
//
// **Exhaustion and non-invertibility are both refusals here, and they are not distinguished.** No
// budget can be principled: arbitrarily close to a fold the convergence ratio goes to 1 and no
// finite number of passes arrives. What can be said is that a real lens does not have a fold inside
// its own frame — an image folded over itself is visible to the eye — so on anything a camera
// produces the loop settles in about five passes and this ceiling never binds. It binds on
// synthetic lenses used to probe the model, and there a refusal near the fold is the right answer
// for a slightly weaker reason than the file would like.
constexpr int kInverseIterations = 5000;

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

// Whether the radial map increases over the *whole* way out to this radius, rather than merely at
// it. The difference is a promise against a coincidence.
//
// `RadialSlope` is a cubic in r^2, and a cubic can dip below zero and come back. With k1 = -1 and
// k2 = +0.3 it is negative between r = 0.650 and r = 1.256 and positive on either side, so asking
// only about the endpoint re-admits radii the image has already folded over: two directions 39
// degrees apart then land on the same pixel, both accepted, and no round-trip check can tell them
// apart because both of them really do project there.
//
// The slope is 1 at the optical centre, so it is enough to check the endpoint and every local
// minimum strictly inside the interval. Those sit at the roots of the slope's own derivative,
// 3k1 + 10k2 u + 21k3 u^2 in u = r^2 — a quadratic at worst, so there are at most two to try.
// The Jacobian determinant of the distortion map at a normalised point.
//
// Distortion maps the plane to the plane, and where the determinant stops being positive the map
// has folded — two neighbouring points land on one. For a purely radial lens this factors exactly
// into `Radial(r2) * RadialSlope(r2)`, which is why the radial case can be settled in closed form
// below; `p1` and `p2` are not radial and nothing about the radial polynomial constrains them.
double DistortionJacobian(const Intrinsics& lens, double xn, double yn) {
  const double r2 = xn * xn + yn * yn;
  const double radial = Radial(lens, r2);
  const double dRadial = lens.k1 + r2 * (2.0 * lens.k2 + r2 * 3.0 * lens.k3);   // d(radial)/d(r2)
  const double dxdx = radial + 2.0 * xn * xn * dRadial + 2.0 * lens.p1 * yn + 6.0 * lens.p2 * xn;
  const double dydy = radial + 2.0 * yn * yn * dRadial + 6.0 * lens.p1 * yn + 2.0 * lens.p2 * xn;
  // The map is a gradient field, so the two off-diagonal terms are equal.
  const double cross = 2.0 * xn * yn * dRadial + 2.0 * lens.p1 * xn + 2.0 * lens.p2 * yn;
  return dxdx * dydy - cross * cross;
}

bool RadialMapIncreasesUpTo(const Intrinsics& lens, double r2) {
  if (!(RadialSlope(lens, r2) > 0.0)) return false;

  const double a = 21.0 * lens.k3;
  const double b = 10.0 * lens.k2;
  const double c = 3.0 * lens.k1;

  // A turning point outside (0, r2) says nothing about this interval. A non-finite one compares
  // false on both bounds and is treated the same way, which is right: it is further out than any
  // radius we were asked about.
  const auto climbsAt = [&](double u) {
    if (!(u > 0.0 && u < r2)) return true;
    return RadialSlope(lens, u) > 0.0;
  };

  if (a == 0.0) {
    // The slope is affine in u (or constant), so it cannot turn: the endpoint settled it.
    if (b == 0.0) return true;
    return climbsAt(-c / b);
  }
  const double discriminant = b * b - 4.0 * a * c;
  if (!(discriminant >= 0.0)) return true;   // never turns, so the endpoint settled it
  const double root = std::sqrt(discriminant);
  return climbsAt((-b + root) / (2.0 * a)) && climbsAt((-b - root) / (2.0 * a));
}

// Whether the distortion is invertible everywhere on the straight line from the optical centre out
// to this point — the property `Project` promises and `Unproject` relies on.
//
// The radial part is exact and closed-form. The tangential part is not: `p1` and `p2` break the
// reduction to one dimension, and there is no cheap closed form for where a full Brown-Conrady map
// first folds. So the determinant is **sampled** along the ray. A fold thinner than a sixty-fourth
// of the ray would slip through, and a lens with one is outside what this model can describe —
// which is a limit worth stating rather than a guarantee worth implying.
//
// The alternative was to keep promising injectivity and check only the radial half, which is what
// this did until a reviewer produced a lens answering an in-frame pixel with a bearing 86 degrees
// wrong. The round trip cannot catch that: the answer is a genuine preimage, just the wrong one of
// two, so it projects back exactly.
bool DistortionInvertsAlongTheRay(const Intrinsics& lens, double xn, double yn) {
  if (!RadialMapIncreasesUpTo(lens, xn * xn + yn * yn)) return false;
  if (lens.p1 == 0.0 && lens.p2 == 0.0) return true;   // radial only, and that was exact

  constexpr int kRaySamples = 64;
  for (int i = 1; i <= kRaySamples; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(kRaySamples);
    if (!(DistortionJacobian(lens, xn * t, yn * t) > 0.0)) return false;
  }
  return true;
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
  if (lens.width <= 0 || lens.height <= 0) return false;
  // The optical centre has to be inside the image. Not fussiness about realism: each half-angle in
  // HorizontalFovDeg is measured from the centre out to an edge, so a centre outside makes one of
  // them negative and cancels the other — far enough out, the two atans round to the same double
  // just under pi/2 and the field of view comes back as exactly 0, which is the contract's word for
  // "will not say". A sentinel a usable lens can produce is not a sentinel.
  return lens.cx > 0.0 && lens.cx < static_cast<double>(lens.width) && lens.cy > 0.0 &&
         lens.cy < static_cast<double>(lens.height);
}

Intrinsics LensFromFieldOfView(double horizontalFovDeg, double verticalFovDeg, int32_t width,
                               int32_t height) {
  // A default Intrinsics is unusable, so every refusal below is a plain return: there is no way to
  // answer this wrongly and have the answer still look like a lens.
  Intrinsics lens;
  if (!std::isfinite(horizontalFovDeg) || !std::isfinite(verticalFovDeg)) return lens;
  if (horizontalFovDeg <= 0.0 || verticalFovDeg <= 0.0) return lens;
  // At 180 degrees the half-angle's tangent is infinite and a rectilinear lens has stopped
  // existing. RingsCoveragePlannerEngine is very slightly more permissive — it refuses *wider* than
  // 180 and lets exactly 180 through — so this is stricter than the planner rather than the same
  // rule, and 180 itself is the one angle they disagree about.
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

// Measured by asking the model where the two opposite edges of the frame actually look, rather than
// by reading the focal length. Two things fall out of that which arithmetic on fx would get wrong.
//
// Distortion counts. Barrel distortion bends the edge of the frame outward, so the lens sees more
// of the world than its focal length implies: on the test suite's distorted lens, fx alone says 66
// degrees and the frame subtends nearly 73. An earlier version of this function computed two
// half-angles from fx and cx and was carefully right about a 2.1-degree decentring error while
// being 6.9 degrees wrong about distortion.
//
// And the decentring still comes out right for free, because the two half-angles are simply the two
// edges: a centre that is not in the middle gives unequal halves whose sum is smaller, since atan
// is concave and a sum of two of them is largest when they are equal.
//
// An edge with no preimage — a barrel strong enough that the frame's own corner is past the fold —
// answers 0, the contract's word for "will not say" (types.h). That is the honest answer: the lens
// has no left-hand side to measure to.
double HorizontalFovDeg(const Intrinsics& lens) {
  if (!IsUsableLens(lens)) return 0.0;
  const UnprojectedDirection left = Unproject(lens, Pixel{0.0, lens.cy});
  const UnprojectedDirection right =
      Unproject(lens, Pixel{static_cast<double>(lens.width), lens.cy});
  if (!left.valid || !right.valid) return 0.0;
  return AngleBetweenDirections(left.direction, right.direction) * kRadToDeg;
}

double VerticalFovDeg(const Intrinsics& lens) {
  if (!IsUsableLens(lens)) return 0.0;
  const UnprojectedDirection top = Unproject(lens, Pixel{lens.cx, 0.0});
  const UnprojectedDirection bottom =
      Unproject(lens, Pixel{lens.cx, static_cast<double>(lens.height)});
  if (!top.valid || !bottom.valid) return 0.0;
  return AngleBetweenDirections(top.direction, bottom.direction) * kRadToDeg;
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
  if (!DistortionInvertsAlongTheRay(lens, xn, yn)) return out;
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
