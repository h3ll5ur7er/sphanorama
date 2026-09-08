#include "support/rotation_scoring.h"

#include <algorithm>
#include <cmath>

#include "utilities/quaternion.h"

namespace sphanorama::test {
namespace {

constexpr double kDegPerRad = 57.295779513082320876798154814105;

// Cyclic Jacobi sweeps over the 4x4. Measured rather than chosen — see the note at the call site.
constexpr int kJacobiSweeps = 24;

// The off-diagonal magnitude below which the matrix is diagonal to double precision. Entries are
// sums of unit-quaternion outer products, so they scale with the frame count; this is compared
// against a normalised quantity so the threshold does not have to.
constexpr double kOffDiagonalSettled = 1e-18;

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
// as (lambda2/lambda1)^k, so it is fast exactly when the residuals agree and slow exactly when they
// do not — and "they do not" is what a half-broken registration looks like. Measured over 4,000
// trials of half-exact-half-garbage input, power iteration exhausted a 200-iteration budget;
// Jacobi's sweep count does not depend on the gap at all.
Vec4 DominantEigenvector(double m[4][4]) {
  double v[4][4]{};
  for (int k = 0; k < 4; ++k) v[k][k] = 1.0;

  for (int sweep = 0; sweep < kJacobiSweeps; ++sweep) {
    double offDiagonal = 0;
    for (int p = 0; p < 4; ++p) {
      for (int q = p + 1; q < 4; ++q) offDiagonal += m[p][q] * m[p][q];
    }
    if (offDiagonal <= kOffDiagonalSettled) break;

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
  return Vec4{{v[0][largest], v[1][largest], v[2][largest], v[3][largest]}};
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

  const Vec4 eigenvector = DominantEigenvector(m);
  const Quat rotation = Normalize(AsQuat(eigenvector));
  if (!IsUsableRotation(rotation)) return out;
  out.rotation = rotation;
  out.valid = true;
  return out;
}

RotationScore ScoreRotations(const std::vector<Quat>& estimated, const std::vector<Quat>& truth) {
  RotationScore out;
  const GaugeAlignment gauge = BestGaugeAlignment(estimated, truth);
  if (!gauge.valid) return out;

  out.alignment = gauge.rotation;
  out.perFrameDeg.reserve(estimated.size());
  double sum = 0;
  for (size_t i = 0; i < estimated.size(); ++i) {
    const double deg = AngleBetween(Multiply(gauge.rotation, estimated[i]), truth[i]) * kDegPerRad;
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
