#include "utilities/camera_model.h"

#include <cmath>

#include "utilities/quaternion.h"

namespace sphanorama {

// **On the layering of the guards below.** Many refusals here are individually removable without a
// test going red. Measured over all **twenty-nine** of them, one at a time, against the whole suite:
// nine fail a test outright, sixteen leave it green, and four cannot be neutralised that way without
// breaking the build — those four were sabotaged individually and every one is caught. So **thirteen
// caught, sixteen not.**
//
// This count has been wrong twice. It first said "thirteen of fourteen, the only exception being the
// depth test", from a hand-picked subset. Corrected to "ten of twenty-six", which was right for the
// code at the time and went stale the moment the solver changed. A note about measurement discipline
// is worth nothing if the number in it is not re-measured with the thing it describes.
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
// Two of them are load-bearing rather than defensive, and both took a sweep to find:
//
//   - `Unproject`'s `check.valid`. A refused `ProjectedPixel` carries the pixel `(0, 0)`, so folding
//     that test into the tolerance comparison below it returns a confident wrong direction for an
//     input pixel of exactly `(0, 0)`, and for no other input.
//   - `Project`'s `isfinite(u, v)`, which is the counterexample to the paragraph above and the
//     reason it is stated as a policy rather than as a proof. With `k3 = 1e293` — finite, and a lens
//     `IsUsableLens` vouches for — every intermediate is finite and positive (`r2` 1e4, `radial`
//     1e305, `xd` 1e307) and the overflow happens at `fx * xd` alone. No NaN, no later guard, no
//     backstop. Without it the answer is `valid = true, pixel = (inf, 640)`.
//
// Both now have a test. The remaining sixteen do not, and that is the price of the policy rather
// than evidence for it.

namespace {

constexpr double kDegToRad = 0.017453292519943295;
constexpr double kRadToDeg = 57.29577951308232;

// How far an unprojected direction may land from the pixel it came from when projected back, in
// normalised image units. At the focal lengths a phone has this is under a millionth of a pixel —
// tight enough that a distortion which does not invert is caught, loose enough that an ordinary
// one is never refused for rounding.
constexpr double kInverseToleranceNormalised = 1e-9;

// Brown-Conrady is inverted by damped Newton, which stops as soon as it has stopped moving.
//
// Newton, not fixed-point iteration, and the difference is not performance.
//
// The obvious inverse for Brown-Conrady is the fixed point xn <- (xd - tangential(xn)) / radial(xn).
// It is what this file shipped first, and it is wrong for the problem: its multiplier at the
// solution is |2u.R'(u)/R(u)|, which for a **pure k1** lens drops below 1 exactly where the map
// stops inverting — the two thresholds coincide, which is why the error survived a round of review
// against radial-only lenses. Add k2 and they separate. A reviewer produced a 115-degree lens with
// k1 = -0.3, k2 = 0.1 whose radial slope has discriminant -1.19 and therefore folds *nowhere*: every
// pixel has exactly one preimage, and the iteration entered a 2-cycle 0.7 normalised units away
// from it and stayed there. A quarter of that frame was refused, at 500x the cost of an answer.
//
// Newton uses the Jacobian, converges quadratically, and reaches solutions the fixed point cannot
// approach. Measured over every accepted in-frame pixel: a median of 1 pass undistorted, 4 to 5 on
// the barrel, ultra-wide and pincushion lenses in this file's tests, and a longest tail of 12. So
// twenty is a ceiling with room in it rather than a number anything reaches, and a refusal after
// twenty is a genuine failure to converge rather than a budget running out.
//
// (An earlier draft said "four or five" with no tail, and a reviewer measured the tail at exactly
// twenty — on the undamped version. Damping shortened it. Both numbers were honestly arrived at and
// only one of them describes what ships, which is the argument for re-measuring after every change
// rather than carrying a figure forward.)
constexpr int kInverseIterations = 20;

// How many halvings a damped step may take, and how many times the initial guess may be pulled
// toward the optical centre before the model is defined where it stands. Thirty halvings is a factor
// of a billion: past that the step is not going to become useful, and the round-trip check below is
// what decides whether where we stopped is an answer.
constexpr int kBacktrackSteps = 30;

// A Newton step this small has arrived: the method roughly squares its error each pass, so the step
// and the remaining error are the same size to within that squaring, and a step of 1e-14 leaves
// nothing the 1e-9 tolerance below would object to. The early exit is an optimisation here rather
// than a judgement — this constant was three orders tighter than the tolerance to cover a *linearly*
// converging sequence, which could still sit far from its limit while stepping slowly, and that is
// no longer the method in use.
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

// The Jacobian determinant of the distortion map at a normalised point.
//
// Distortion maps the plane to the plane, and where the determinant stops being positive the map
// has folded — two neighbouring points land on one. For a purely radial lens this factors exactly
// into `Radial(r2) * RadialSlope(r2)`, which is why the radial case can be settled in closed form
// below; `p1` and `p2` are not radial and nothing about the radial polynomial constrains them.
// Where the distortion sends a point, and its Jacobian there, in one evaluation.
//
// One evaluation because there were two: the solver rebuilt these four numbers inline while
// `DistortionJacobian` computed the determinant separately, and a reviewer pointed out that a fact
// held in two places will drift — the fold test would then disagree with the solver about where the
// fold is, which is precisely the disagreement this file exists to prevent.
struct Distorted {
  double x = 0, y = 0;          // the distorted point
  double dxdx = 0, dydy = 0;    // the Jacobian's diagonal
  double cross = 0;             // both off-diagonal terms — the map is a gradient field, so they
                                // are equal, which is why there is one of them
  double radial = 0;
  double determinant = 0;
};

Distorted DistortAt(const Intrinsics& lens, double xn, double yn) {
  Distorted at;
  const double r2 = xn * xn + yn * yn;
  at.radial = Radial(lens, r2);
  at.x = xn * at.radial + 2.0 * lens.p1 * xn * yn + lens.p2 * (r2 + 2.0 * xn * xn);
  at.y = yn * at.radial + lens.p1 * (r2 + 2.0 * yn * yn) + 2.0 * lens.p2 * xn * yn;
  const double dRadial = lens.k1 + r2 * (2.0 * lens.k2 + r2 * 3.0 * lens.k3);   // d(radial)/d(r2)
  at.dxdx = at.radial + 2.0 * xn * xn * dRadial + 2.0 * lens.p1 * yn + 6.0 * lens.p2 * xn;
  at.dydy = at.radial + 2.0 * yn * yn * dRadial + 6.0 * lens.p1 * yn + 2.0 * lens.p2 * xn;
  at.cross = 2.0 * xn * yn * dRadial + 2.0 * lens.p1 * xn + 2.0 * lens.p2 * yn;
  at.determinant = at.dxdx * at.dydy - at.cross * at.cross;
  return at;
}

double DistortionJacobian(const Intrinsics& lens, double xn, double yn) {
  return DistortAt(lens, xn, yn).determinant;
}

// Whether the model is defined at a point at all — somewhere a Newton step may stand.
bool DefinedAt(const Distorted& at) { return at.radial > 0.0 && at.determinant > 0.0; }

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
  // `b*b` and `4ac` can each overflow to infinity, and `inf - inf` is NaN. A NaN is not "no real
  // roots" — it is "I could not tell", and taking the no-roots exit on it skips both interior
  // checks and re-admits the folded state this function exists to close. Refuse instead: a lens
  // whose coefficients overflow a discriminant is not one this model can vouch for.
  if (!std::isfinite(discriminant)) return false;
  if (discriminant < 0.0) return true;   // never turns, so the endpoint settled it
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
  // The optical centre has to be inside the image.
  //
  // The original reason was a sentinel collision in a version of `HorizontalFovDeg` that computed
  // two atans from `cx` and `fx`: far enough out, both rounded to the same double just under pi/2
  // and the field of view came back as exactly 0, which is the contract's word for "will not say".
  // That arithmetic is gone — the angle is measured through the model now — so this guard is kept on
  // its own merits rather than that one, and they are: a lens whose optical centre is outside its
  // own image is not something the fold reasoning, the field of view or the projection below were
  // written to describe, and refusing it up front is cheaper than reasoning about each in turn.
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
// answers 0, the contract's word for "will not say" (types.h). That is the honest answer when it is
// true: the lens has no left-hand side to measure to.
//
// It is worth knowing that this sentence was briefly false. While `Unproject` used a fixed point it
// also answered 0 for an ultra-wide lens whose edge had a perfectly good preimage the solver could
// not reach — and a comfortable explanation for a wrong answer is exactly what stops the next person
// looking. `RingsCoveragePlannerEngine` refuses to tessellate on a 0, so that reached further than
// the sentence suggested.
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

  // **The guess is not the answer, and the guards must not judge it as one.** Newton starts from the
  // distorted coordinate. On a barrel lens (k1 < 0) that sits *inside* the solution and the
  // iteration walks outward, which is why every lens in this file's tests was fine — all of them had
  // k1 <= 0. On a pincushion lens it sits outside, and it can already be past the fold. Refusing
  // there refuses a pixel `Project` itself produced. Pull the guess in until the model is defined
  // where it stands; the optical centre always qualifies, since radial and the determinant are both
  // 1 at r = 0, so this terminates.
  for (int i = 0; i < kBacktrackSteps; ++i) {
    if (DefinedAt(DistortAt(lens, xn, yn))) break;
    xn *= 0.5;
    yn *= 0.5;
  }

  for (int i = 0; i < kInverseIterations; ++i) {
    const Distorted at = DistortAt(lens, xn, yn);
    if (!DefinedAt(at)) return out;

    const double residualX = at.x - xd;
    const double residualY = at.y - yd;
    const double residual = residualX * residualX + residualY * residualY;

    const double fullX = -(at.dydy * residualX - at.cross * residualY) / at.determinant;
    const double fullY = -(at.dxdx * residualY - at.cross * residualX) / at.determinant;
    if (!std::isfinite(fullX) || !std::isfinite(fullY)) return out;

    // **Damped, because a full step can leap the fold.** Newton aims at where the *linearised* map
    // sends the residual to zero, and near a fold that aim overshoots into territory the model does
    // not describe. Halving until the trial point is both defined and closer than where we stand is
    // the standard remedy, and it is what makes the pincushion case converge instead of refusing.
    // Requiring the residual to *decrease* rather than merely be defined is what stops it cycling.
    double scale = 1.0;
    bool stepped = false;
    double stepX = 0.0;
    double stepY = 0.0;
    for (int b = 0; b < kBacktrackSteps; ++b) {
      stepX = scale * fullX;
      stepY = scale * fullY;
      const double tryX = xn + stepX;
      const double tryY = yn + stepY;
      const Distorted trial = DistortAt(lens, tryX, tryY);
      if (DefinedAt(trial)) {
        const double dx = trial.x - xd;
        const double dy = trial.y - yd;
        if (dx * dx + dy * dy < residual) {
          xn = tryX;
          yn = tryY;
          stepped = true;
          break;
        }
      }
      scale *= 0.5;
    }
    // Nowhere better to stand. Not a refusal on its own — the round-trip check below decides
    // whether where we stopped is an answer.
    if (!stepped) break;
    if (stepX * stepX + stepY * stepY <= kSettledStepNormalised * kSettledStepNormalised) break;
  }

  const Vec3 direction = Normalize(Vec3{xn, -yn, -1.0});

  // Checked against the forward model, and by calling `Project` rather than by repeating its
  // arithmetic here. Two reasons: an iterate that has stopped moving has not necessarily stopped at
  // the right place — the damping can run out of halvings and leave it where it stands — and a pixel
  // past the fold has to fall out of this check, which is a property of `Project`, so asking
  // `Project` is the only way the two agree on where the fold is.
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
