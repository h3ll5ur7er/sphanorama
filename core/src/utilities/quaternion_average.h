#pragma once

#include <span>

#include "sphanorama/types.h"

namespace sphanorama {

// The one rotation that best represents many, with an optional weight on each.
//
// **"Best" is the chordal sense**: it maximises the sum of weighted squared quaternion dot products
// against the inputs, which is Markley's average and is computed as the principal eigenvector of the
// weighted outer-product sum. That is not identical to minimising the sum of squared *angles* — the
// two agree to second order, so they diverge only when the inputs are spread far enough apart that
// no single rotation was going to represent them anyway.
//
// **Sign is not a special case.** The outer product is invariant under `q -> -q`, which is the same
// rotation, so nothing here has to decide which hemisphere an input belongs in. The arithmetic mean
// people reach for first does have to, and gets the zero quaternion when it guesses wrong.
//
// This is shared rather than owned by one caller because there are two, wanting the same maths for
// different reasons: the gauge alignment in `core/test/support/rotation_scoring` averages residuals
// to find the common rotation between two frame sets, and the rotation solver averages a frame's
// neighbours' predictions of where it should sit. A second copy of a 4x4 eigensolver is exactly the
// kind of duplicate that drifts — and the choice of eigensolver here is a measured one (ADR 0049),
// so a second copy would also be a second place for that measurement to go stale.

struct QuaternionAverage {
  Quat rotation;

  // False when the top two eigenvalues are equal, so more than one rotation maximises the objective
  // and the one returned is an arbitrary member of a continuum. The answer is still *a* maximiser;
  // what is not trustworthy is treating it as *the* one. Two rotations exactly a half turn apart are
  // the reachable case.
  bool isUnique = true;

  bool valid = false;
};

// `weights` is either empty, meaning every rotation counts the same, or parallel to `rotations`.
//
// Weights are evidence rather than a distribution: only their ratios matter, so raw inlier counts
// may be passed without normalising. A weight of zero removes its rotation from the average without
// removing it from the input, which is what a caller holding a parallel array wants.
//
// Every way of having no answer is `valid == false` rather than a plausible rotation, because the
// identity is a perfectly ordinary rotation that a caller cannot tell from a measurement: no
// rotations, a weight span that is neither empty nor the right length, an input that is not a
// rotation (see `IsUsableRotation`), a weight that is negative or not finite, or every weight zero.
QuaternionAverage AverageQuaternions(std::span<const Quat> rotations,
                                     std::span<const double> weights);

}  // namespace sphanorama
