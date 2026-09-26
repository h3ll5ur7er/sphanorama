#pragma once

#include <cstdint>
#include <limits>

#include "sphanorama/types.h"

namespace sphanorama {

// The lens a device keeps between captures, and how one more capture amends it (ADR 0067).
//
// The pairs' noise and a lens model's error are kept apart because they combine differently: noise
// averages down over captures, and the error of misreading the same lens the same way does not
// (ADR 0066). `lens.focalUncertainty` is the two combined, which is what `Refine` weighs a new fit
// against; it is written here and never read.
struct KeptLens {
  Intrinsics lens;
  double noise = std::numeric_limits<double>::infinity();
  double modelError = std::numeric_limits<double>::infinity();
  // Zero is nothing kept, and then nothing else here is read: the first measurement is taken whole.
  int32_t captures = 0;
};

// Amended by every capture whose least `Refine` found precise — `focalScale` above zero — whether
// or not it took the fit for that capture's rotations, and in its focal length alone. The two are
// combined in the natural log of the focal length, read as a fraction of the long edge so a capture
// at another size amends the same lens, at the one weight that leaves the least uncertainty.
//
// `InvalidArgument` for a kept lens that cannot project or whose figures are not finite and at
// least zero, a negative count, a scale that is not finite and at least zero, a measuring capture
// whose lens cannot project or whose figures are not finite and at least zero, and a capture whose
// frame is another shape than the kept lens's.
Result<KeptLens> AmendKeptLens(const KeptLens& kept, const GlobalSolution& capture);

}  // namespace sphanorama
