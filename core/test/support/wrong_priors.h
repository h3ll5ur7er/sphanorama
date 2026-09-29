#pragma once
#include <cmath>
#include <numbers>
#include <vector>

#include "sphanorama/types.h"
#include "utilities/quaternion.h"

namespace sphanorama::test {

// What a working phone would hand registration about a ring whose truth is known: every guess three
// degrees out, the order a fused orientation is out by. Shared because the registration table and
// the preview composed from a registered ring have to be measuring the same solve, and each copy of
// these lines was one edit from measuring a different one.

// The prior a pair is estimated against: the true step from `a` to `b`, turned three degrees about
// x — an axis the ring does not turn about, so the rotation being estimated cannot absorb it.
inline Quat PairPriorThreeDegreesOut(const Quat& a, const Quat& b) {
  const Quat nudge = FromAxisAngle(Vec3{1, 0, 0}, 3.0 * std::numbers::pi / 180.0);
  return Normalize(Multiply(Multiply(Conjugate(b), a), nudge));
}

// A prior for every frame, each three degrees out about an axis of its own, so the priors cannot
// agree on a wrong shape. Anchored, as every burst-captured frame's is (ADR 0044). Lists of two
// lengths answer none rather than the shorter, so a caller that mismatched them solves nothing
// instead of a ring missing its last frames.
inline std::vector<FramePrior> FramePriorsThreeDegreesOut(const std::vector<FrameId>& frames,
                                                          const std::vector<Quat>& truth) {
  std::vector<FramePrior> priors;
  if (frames.size() != truth.size()) return priors;
  for (size_t i = 0; i < truth.size(); ++i) {
    const double at = static_cast<double>(i);
    const Vec3 axis{std::sin(at), std::cos(at), std::sin(2.0 * at)};
    FramePrior prior;
    prior.frame = frames[i];
    prior.pose.orientation =
        Normalize(Multiply(truth[i], FromAxisAngle(axis, 3.0 * std::numbers::pi / 180.0)));
    prior.pose.confidence = 1.0;
    priors.push_back(prior);
  }
  return priors;
}

}  // namespace sphanorama::test
