#pragma once

#include <vector>

#include "sphanorama/types.h"

namespace sphanorama::test {

// How wrong a set of estimated frame rotations is, against rotations we know.
//
// This is the measuring instrument for Phase 2, and it exists before the thing it measures on
// purpose: registration accuracy is invisible to the eye. A rotation a degree out produces a
// panorama that looks fine until the seam, and by then the cause is three stages back. A number
// is the only thing that can tell a regression from a re-tuning.
//
// **The gauge is the whole difficulty.** A panorama is reconstructed from how frames sit relative
// to each other, so the world frame it lands in is arbitrary: turn every estimated rotation by one
// common rotation and you have described the identical panorama in different coordinates. `Quat` is
// device -> world (types.h), so that freedom acts on the left — the estimate {q_i} and the estimate
// {G (x) q_i} are the same reconstruction for any rotation G. Comparing q_i against r_i directly
// would therefore report an arbitrary offset as error, and would report it on every frame at once,
// which looks exactly like a badly broken registration and is in fact a correct one.
//
// So scoring removes the gauge first: find the G that best carries the estimate onto the truth, and
// measure what is left. What is left is the part registration is actually responsible for.
//
// **Two consequences worth knowing before you read a score.**
//
// A one-frame reconstruction always scores zero. With a single pair there is a G that carries the
// estimate exactly onto the truth, so the gauge absorbs the entire error and nothing remains to
// measure. That is not a bug in the scorer, it is what "accuracy" means when there is nothing to be
// accurate *relative to* — and it is why a dataset needs frames that overlap, not merely frames.
//
// A single bad frame smears error onto the good ones, and the median does **not** escape it. This
// was stated the other way round at first — "the median, which an outlier cannot move" — and that
// is wrong for a reason worth understanding, because it is the opposite of how a median usually
// behaves. A median is robust when outliers are extreme *values* it can step over. Here the outlier
// does not contribute an extreme value; it drags the alignment, and every good frame then inherits
// the **same** contamination. The median is therefore not a value the outlier failed to reach — the
// median *is* the contamination.
//
// Measured, with one frame turned by the angle shown and every other frame exact:
//
//     outlier      9 frames   31 frames   61 frames
//        20 deg      2.19        0.63        0.32
//       120 deg      6.59        1.68        0.83
//
// It falls as roughly 1/N but never reaches immunity. At the 30 to 60 cells a real sphere plans,
// one badly placed frame still moves the median by about a degree on its own — the same order as
// any plausible value for the threshold the roadmap's exit criterion has yet to state. So the
// median is the right headline number because it is far steadier than the mean, not because it is
// immune; and `maxDeg` is reported beside it because that is where a single bad frame is actually
// legible.

struct RotationScore {
  std::vector<double> perFrameDeg;   // one per input pair, in the order they were given
  double medianDeg = 0;              // the statistic the roadmap's exit criterion names
  double meanDeg = 0;
  double maxDeg = 0;
  Quat alignment;                    // the gauge that was removed, for a caller that wants to see it

  // False when the residuals leave the top two eigenvalues equal, so more than one rotation
  // maximises the objective and the one returned is an arbitrary member of a continuum. The score
  // is still a valid maximiser; what is not trustworthy is how the error is *distributed* across
  // frames. Two frames exactly 180 degrees apart are the reachable case: the honest reading is
  // 90 degrees each, and depending on input order this reports [0, 180] or [180, 0].
  bool alignmentIsUnique = true;

  bool valid = false;
};

// The rotation that best carries `estimated` onto `truth`.
//
// "Best" is the chordal sense: it maximises the sum of squared quaternion dot products, which is
// Markley's average and is computed as the principal eigenvector of the residuals' outer-product
// sum. That is not identical to minimising the sum of squared *angles* — the two agree to second
// order, so they diverge only when the errors are large enough that the median is the number you
// were going to read anyway. Sign is not a special case: the outer product is invariant under
// q -> -q, which is the same rotation.
//
// Returns the identity with `valid == false` on input that cannot be scored: empty, mismatched
// lengths, or any quaternion that is not a rotation (see `IsUsableRotation`).
struct GaugeAlignment {
  Quat rotation;
  bool isUnique = true;   // see RotationScore::alignmentIsUnique
  bool valid = false;
};
GaugeAlignment BestGaugeAlignment(const std::vector<Quat>& estimated, const std::vector<Quat>& truth);

// Per-frame angular error after the gauge is removed, plus its summary statistics.
//
// `medianDeg` on an even count is the mean of the two middle values, so a two-frame dataset reports
// the same number twice over rather than picking one arbitrarily.
RotationScore ScoreRotations(const std::vector<Quat>& estimated, const std::vector<Quat>& truth);

}  // namespace sphanorama::test
