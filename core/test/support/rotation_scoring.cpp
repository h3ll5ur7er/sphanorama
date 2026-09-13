#include "support/rotation_scoring.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "utilities/quaternion.h"

namespace sphanorama::test {
namespace {

constexpr double kDegPerRad = 180.0 / std::numbers::pi;

// Cyclic Jacobi sweeps over the 4x4. Measured rather than chosen — see the note at the call site.
constexpr int kJacobiSweeps = 24;

// The off-diagonal weight below which the matrix counts as diagonal. Entries are sums of
// unit-quaternion outer products, so they scale with the frame count — M's trace is exactly N —
// and the sum of their squares therefore scales as N squared. An absolute threshold was wrong for
// that reason: measured, the sweep-zero off-diagonal runs from 3.2e-01 at one frame to 3.1e+09 at
// a hundred thousand, so a fixed 1e-18 meant something different at every dataset size. A reviewer
// found the comment claiming a normalisation that was not there. This compares against the squared
// trace, which is the scale the entries actually have, so one threshold means one thing.
constexpr double kOffDiagonalSettled = 1e-30;

// Relative separation below which the top two eigenvalues count as equal, so the maximiser is a
// continuum rather than a rotation. Exact ties are what this is for — two frames 180 degrees apart
// give 1.0 and 1.0 — and a threshold this tight claims only that, not a general conditioning test.
constexpr double kEigenvalueGap = 1e-12;

struct Vec4 {
  double v[4]{};
};

Vec4 AsVec4(const Quat& q) { return Vec4{{q.w, q.x, q.y, q.z}}; }
Quat AsQuat(const Vec4& a) { return Quat{a.v[0], a.v[1], a.v[2], a.v[3]}; }

// The rotation that carries `from` onto `to`. Left-multiplied, because Quat is device -> world and
// the gauge freedom lives in the world frame: `to == residual (x) from`.
Quat Residual(const Quat& from, const Quat& to) {
  return Normalize(Multiply(Normalize(to), Conjugate(from)));
}

bool EveryOneIsARotation(const std::vector<Quat>& frames) {
  return std::all_of(frames.begin(), frames.end(),
                     [](const Quat& q) { return IsUsableRotation(q); });
}

// The eigenvector of the largest eigenvalue of a 4x4 symmetric matrix, by cyclic Jacobi.
//
// Jacobi rather than power iteration, and the difference is not academic. Power iteration converges
// as (lambda2/lambda1)^k, so it is fast when the residuals agree and slow when they do not. Measured
// over 5,000 trials at each frame count: on wholly unrelated estimates it exhausts a 200-iteration
// budget 13.9% of the time at 12 frames and **45.6% at 60** — which is the size a real sphere plans.
// Jacobi does not depend on the gap at all: 5 working sweeps, every regime, every frame count from
// 2 to 60. See ADR 0049, including what the first version of this comment got wrong.
struct Eigen {
  Vec4 vector;
  bool separated = true;   // false when the top two eigenvalues are equal
};

Eigen DominantEigenvector(double m[4][4]) {
  // The scale the off-diagonal test is relative to. M's trace is the frame count, and it is
  // invariant under the rotations below, so this is computed once up front.
  double trace = 0;
  for (int k = 0; k < 4; ++k) trace += m[k][k];
  const double scale = (trace > 0.0) ? trace * trace : 1.0;

  double v[4][4]{};
  for (int k = 0; k < 4; ++k) v[k][k] = 1.0;

  for (int sweep = 0; sweep < kJacobiSweeps; ++sweep) {
    double offDiagonal = 0;
    for (int p = 0; p < 4; ++p) {
      for (int q = p + 1; q < 4; ++q) offDiagonal += m[p][q] * m[p][q];
    }
    if (offDiagonal <= kOffDiagonalSettled * scale) break;

    for (int p = 0; p < 4; ++p) {
      for (int q = p + 1; q < 4; ++q) {
        if (m[p][q] == 0.0) continue;
        const double theta = (m[q][q] - m[p][p]) / (2.0 * m[p][q]);
        const double sign = (theta >= 0.0) ? 1.0 : -1.0;
        const double t = sign / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;

        for (int k = 0; k < 4; ++k) {
          const double mkp = m[k][p];
          const double mkq = m[k][q];
          m[k][p] = c * mkp - s * mkq;
          m[k][q] = s * mkp + c * mkq;
        }
        for (int k = 0; k < 4; ++k) {
          const double mpk = m[p][k];
          const double mqk = m[q][k];
          m[p][k] = c * mpk - s * mqk;
          m[q][k] = s * mpk + c * mqk;
        }
        for (int k = 0; k < 4; ++k) {
          const double vkp = v[k][p];
          const double vkq = v[k][q];
          v[k][p] = c * vkp - s * vkq;
          v[k][q] = s * vkp + c * vkq;
        }
      }
    }
  }

  int largest = 0;
  for (int k = 1; k < 4; ++k) {
    if (m[k][k] > m[largest][largest]) largest = k;
  }

  // Whether the answer is the maximiser or *a* maximiser. When the top two eigenvalues are equal
  // every unit vector in their eigenspace maximises the objective equally, and which one comes back
  // is decided by nothing better than the order `largest` scanned in — so the caller is told rather
  // than handed an arbitrary choice dressed as the answer.
  int second = -1;
  for (int k = 0; k < 4; ++k) {
    if (k == largest) continue;
    if (second < 0 || m[k][k] > m[second][second]) second = k;
  }
  const double top = m[largest][largest];
  const bool separated = !(top > 0.0) || (top - m[second][second]) > kEigenvalueGap * top;

  return Eigen{Vec4{{v[0][largest], v[1][largest], v[2][largest], v[3][largest]}}, separated};
}

}  // namespace

GaugeAlignment BestGaugeAlignment(const std::vector<Quat>& estimated,
                                  const std::vector<Quat>& truth) {
  GaugeAlignment out;
  if (estimated.empty() || estimated.size() != truth.size()) return out;
  if (!EveryOneIsARotation(estimated) || !EveryOneIsARotation(truth)) return out;

  // Markley's average: the alignment we want maximises the sum of squared dot products between the
  // aligned estimate and the truth, and that sum is g^T M g for M the residuals' outer-product sum.
  // The maximiser over unit vectors is M's principal eigenvector.
  //
  // Squaring is what makes the sign question disappear rather than needing handling: e and -e are
  // the same rotation and contribute the same outer product, so nothing here has to decide which
  // hemisphere a residual belongs in.
  double m[4][4]{};
  for (size_t i = 0; i < estimated.size(); ++i) {
    const Vec4 e = AsVec4(Residual(estimated[i], truth[i]));
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) m[r][c] += e.v[r] * e.v[c];
    }
  }

  const Eigen eigen = DominantEigenvector(m);

  // No usability check on the result, and its absence is deliberate. Jacobi's eigenvector columns
  // are orthonormal by construction — instrumented, they come back unit to within 2 ulp on every
  // call — so a guard here could never fire, and this repository has a rule against keeping guards
  // that cannot, because they read as protection to the next person and are not. What decides a
  // refusal is the input gate above; once the input is a set of rotations, there is an answer.
  const Quat rotation = Normalize(AsQuat(eigen.vector));
  out.rotation = rotation;
  out.isUnique = eigen.separated;
  out.valid = true;
  return out;
}

RotationScore ScoreRotations(const std::vector<Quat>& estimated, const std::vector<Quat>& truth) {
  RotationScore out;
  const GaugeAlignment gauge = BestGaugeAlignment(estimated, truth);
  if (!gauge.valid) return out;

  out.alignment = gauge.rotation;
  out.alignmentIsUnique = gauge.isUnique;
  out.perFrameDeg.reserve(estimated.size());
  double sum = 0;
  for (size_t i = 0; i < estimated.size(); ++i) {
    // Normalised before the product, which `Residual` already does on the same value. A reviewer
    // read this line as an overflow: `IsUsableRotation` admits norms up to sqrt(DBL_MAX), and a
    // product whose squares overflow would make `Normalize` substitute the identity and measure the
    // angle against that. It is not reachable — `Multiply` is exactly norm-multiplicative, so the
    // gate's own `Norm` and this product overflow at precisely the same threshold, and 133,266
    // gate-passing quaternions straddling that boundary produced zero overflows and zero changed
    // answers. But the line was correct only because `AngleBetween` normalises internally, which is
    // a fact about a different file that nothing here asserts; an `AngleBetween` optimised to
    // assume unit input would break this silently, and in the under-reporting direction.
    const Quat aligned = Normalize(Multiply(gauge.rotation, Normalize(estimated[i])));
    const double deg = AngleBetween(aligned, truth[i]) * kDegPerRad;
    out.perFrameDeg.push_back(deg);
    sum += deg;
    out.maxDeg = std::max(out.maxDeg, deg);
  }
  out.meanDeg = sum / static_cast<double>(out.perFrameDeg.size());

  std::vector<double> sorted = out.perFrameDeg;
  std::sort(sorted.begin(), sorted.end());
  const size_t half = sorted.size() / 2;
  out.medianDeg = (sorted.size() % 2 == 1) ? sorted[half] : (sorted[half - 1] + sorted[half]) * 0.5;

  out.valid = true;
  return out;
}

}  // namespace sphanorama::test
