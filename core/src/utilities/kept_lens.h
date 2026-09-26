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
// against.
struct KeptLens {
  Intrinsics lens;
  double noise = std::numeric_limits<double>::infinity();
  double modelError = std::numeric_limits<double>::infinity();
  // Nothing is kept until the first fitted capture; before that `lens` is whatever was assumed.
  int32_t captures = 0;
};

// Only a capture `Refine` fitted amends it, and only its focal length. Captures are weighed by
// their own noise, in the natural log of the focal length, read as a fraction of the long edge so a
// capture at another size amends the same lens.
KeptLens AmendKeptLens(const KeptLens& kept, const GlobalSolution& capture);

}  // namespace sphanorama
