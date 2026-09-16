#include "utilities/quaternion_average.h"

#include <cmath>

#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

// Cyclic Jacobi sweeps over the 4x4. Measured rather than chosen — see `DominantEigenvector`.
constexpr int kJacobiSweeps = 24;

// The off-diagonal weight below which the matrix counts as diagonal. Entries are sums of weighted
// unit-quaternion outer products, so they scale with the weight total — M's trace is exactly that
// total — and the sum of their squares therefore scales as its square. An absolute threshold would
// mean something different at every input size and every weight scale; comparing against the
// squared trace is what makes one threshold mean one thing.
constexpr double kOffDiagonalSettled = 1e-30;

// Relative separation below which the top two eigenvalues count as equal, so the maximiser is a
// continuum rather than a rotation. Exact ties are what this is for — two rotations 180 degrees
// apart give equal eigenvalues — and a threshold this tight claims only that, not a general
// conditioning test.
constexpr double kEigenvalueGap = 1e-12;

struct Eigen {
  double vector[4]{};
  bool separated = true;
};

// The eigenvector of the largest eigenvalue of a 4x4 symmetric matrix, by cyclic Jacobi.
//
// Jacobi rather than power iteration, and the difference is not academic. Power iteration converges
// as (lambda2/lambda1)^k, so it is fast when the inputs agree and slow when they do not. Measured
// over 5,000 trials at each size: on wholly unrelated rotations it exhausts a 200-iteration budget
// 13.9% of the time at 12 inputs and **45.6% at 60** — which is the size a real sphere plans. Jacobi
// does not depend on the gap at all, and that is the whole reason it is here. The count is
// **typically 4 working sweeps and occasionally 6**, at every size from 2 to 60, against a budget of
// 24. Measured over 200,000 trials per size: at 60, 5,820 threes, 173,780 fours, 20,398 fives and 2
// sixes. ADR 0049 holds the decision and the first, wrong, version of this measurement.
Eigen DominantEigenvector(double m[4][4]) {
  // The scale the off-diagonal test is relative to. M's trace is the weight total, and it is
  // invariant under the rotations below, so it is computed once up front.
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

  return Eigen{{v[0][largest], v[1][largest], v[2][largest], v[3][largest]}, separated};
}

}  // namespace

QuaternionAverage AverageQuaternions(std::span<const Quat> rotations,
                                     std::span<const double> weights) {
  QuaternionAverage out;
  if (rotations.empty()) return out;
  if (!weights.empty() && weights.size() != rotations.size()) return out;

  // The whole input is checked before any of it is summed, so a refusal is decided by the input
  // rather than by how far the loop got — and so a caller cannot get a partial average of the
  // prefix that happened to be well formed.
  for (const Quat& q : rotations) {
    if (!IsUsableRotation(q)) return out;
  }
  double weightTotal = 0;
  for (const double w : weights) {
    if (!std::isfinite(w) || w < 0.0) return out;
    weightTotal += w;
  }
  if (!weights.empty() && !(weightTotal > 0.0)) return out;

  // Markley's average: the rotation we want maximises the weighted sum of squared dot products
  // against the inputs, and that sum is `g^T M g` for M the weighted outer-product sum. The
  // maximiser over unit vectors is M's principal eigenvector.
  //
  // Normalising each input is what makes the weights mean what they say: an unnormalised quaternion
  // contributes its squared norm as a second, unasked-for weight, and `IsUsableRotation` admits
  // norms far from one.
  double m[4][4]{};
  for (size_t i = 0; i < rotations.size(); ++i) {
    const Quat unit = Normalize(rotations[i]);
    const double e[4]{unit.w, unit.x, unit.y, unit.z};
    const double weight = weights.empty() ? 1.0 : weights[i];
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) m[r][c] += weight * e[r] * e[c];
    }
  }

  const Eigen eigen = DominantEigenvector(m);

  // No usability check on the result, and its absence is deliberate. Jacobi's eigenvector columns
  // are orthonormal by construction — instrumented, they come back unit to within 2 ulp on every
  // call — so a guard here could never fire, and this repository has a rule against keeping guards
  // that cannot, because they read as protection to the next person and are not. What decides a
  // refusal is the input gate above; once the input is a set of rotations with some weight on them,
  // there is an answer.
  out.rotation = Normalize(Quat{eigen.vector[0], eigen.vector[1], eigen.vector[2], eigen.vector[3]});
  out.isUnique = eigen.separated;
  out.valid = true;
  return out;
}

}  // namespace sphanorama
