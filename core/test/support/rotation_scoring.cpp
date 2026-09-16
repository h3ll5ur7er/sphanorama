#include "support/rotation_scoring.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "utilities/quaternion.h"
#include "utilities/quaternion_average.h"

namespace sphanorama::test {
namespace {

constexpr double kDegPerRad = 180.0 / std::numbers::pi;

// The rotation that carries `from` onto `to`. Left-multiplied, because Quat is device -> world and
// the gauge freedom lives in the world frame: `to == residual (x) from`.
Quat Residual(const Quat& from, const Quat& to) {
  return Normalize(Multiply(Normalize(to), Conjugate(from)));
}

bool EveryOneIsARotation(const std::vector<Quat>& frames) {
  return std::all_of(frames.begin(), frames.end(),
                     [](const Quat& q) { return IsUsableRotation(q); });
}

}  // namespace

GaugeAlignment BestGaugeAlignment(const std::vector<Quat>& estimated,
                                  const std::vector<Quat>& truth) {
  GaugeAlignment out;
  if (estimated.empty() || estimated.size() != truth.size()) return out;
  if (!EveryOneIsARotation(estimated) || !EveryOneIsARotation(truth)) return out;

  // **The gauge is the average of the residuals**, which is what makes this a call rather than a
  // derivation. The alignment we want maximises the sum of squared dot products between the aligned
  // estimate and the truth; that sum is `g^T M g` for M the residuals' outer-product sum, and its
  // maximiser is M's principal eigenvector — which is precisely Markley's average of the residuals.
  // `AverageQuaternions` is that average, equally weighted, and it carries the eigensolver ADR 0049
  // measured.
  //
  // Unweighted rather than weighted by anything: every frame's disagreement with truth counts the
  // same, because a gauge that believed some frames more than others would report a smaller error
  // for having chosen which frames to look at.
  std::vector<Quat> residuals;
  residuals.reserve(estimated.size());
  for (size_t i = 0; i < estimated.size(); ++i) {
    residuals.push_back(Residual(estimated[i], truth[i]));
  }

  const QuaternionAverage average = AverageQuaternions(residuals, {});

  // Not a guard that cannot fire, and worth saying which it is. The input gate above admits exactly
  // what `AverageQuaternions` admits — a non-empty set of usable rotations, no weights — so this
  // cannot be false today. It is here because the two gates are in different files now: a refusal
  // this one learns to make is a refusal this function has to pass on rather than average past.
  if (!average.valid) return out;

  out.rotation = average.rotation;
  out.isUnique = average.isUnique;
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
