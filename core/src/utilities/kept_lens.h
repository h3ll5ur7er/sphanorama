#pragma once

#include <cstdint>
#include <limits>

#include "sphanorama/types.h"

namespace sphanorama {

// The lens a device keeps between captures, and how one more capture amends it (ADR 0067).
//
// The pairs' noise and a lens model's error are kept apart because they combine differently: noise
// averages down over captures, and the error of misreading the same lens the same way does not
// (ADR 0066). `lens.focalUncertainty` is the two combined, written for whoever reads the lens but
// never read here: a document need not carry it, and `KeptLensFor` derives it again.
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
// whose lens cannot project or whose figures are not finite and at least zero, figures on either
// side that combine past the largest double, and a capture whose frame is another shape than the
// kept lens's.
Result<KeptLens> AmendKeptLens(const KeptLens& kept, const GlobalSolution& capture);

// The lens to hand `Refine` for a capture whose frames are `width` by `height`: the kept one, its
// lengths in pixels scaled to that size by the long edge, and its `focalUncertainty` derived from
// the noise and model error kept rather than read from the copy the lens carries. `NotFound` where
// nothing is kept, so the caller starts from its guess; `InvalidArgument` for a frame of another
// shape and for a kept lens `AmendKeptLens` would refuse.
Result<Intrinsics> KeptLensFor(const KeptLens& kept, int32_t width, int32_t height);

}  // namespace sphanorama
