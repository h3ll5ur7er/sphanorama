#pragma once
#include <limits>
#include <numbers>
#include <string>
#include <vector>

#include "sphanorama/engines/registration_engine.h"
#include "support/rotation_scoring.h"
#include "support/synthetic_dataset.h"
#include "support/wrong_priors.h"
#include "utilities/quaternion.h"

namespace sphanorama::test {

// The photograph ring solved with its closing pair, against priors three degrees out: the
// measurement `registration_accuracy_test.cpp` makes natively and `registration_accuracy_wasm.cpp`
// makes in the browser's build (ADR 0069). One copy, so the two are the same solve held to the same
// bounds, and a difference between them is the platform's rather than the harness's.

// The ring: twelve 640 by 480 frames of the photograph. The bounds below mean something at this
// size and not at others — sixteen frames pass them more easily, nine fail ORB — so the WebAssembly
// runner answers `--ring` with it for the renderer and refuses a dataset of any other shape.
inline constexpr int kSolvedRingFrames = 12;
inline constexpr int kSolvedRingWidth = 640;
inline constexpr int kSolvedRingHeight = 480;

// Why these, and what they are measured against: `TheRingSolvedWithItsClosingPair...`.
inline constexpr double kSolvedRingMedianBoundDeg = 0.08;
inline constexpr double kSolvedRingMaxBoundDeg = 0.25;
// The solve faces where the priors agree, since that is the one thing they are there to decide, and
// the scored figures cannot see it: `ScoreRotations` removes the gauge before it measures.
inline constexpr double kSolvedRingFacingBoundDeg = 0.001;

// Every consecutive pair of `sets`, the last with the first, estimated against the true step three
// degrees out, then refined against every frame's prior three degrees out. `sets[i]` is
// `frames[i]`'s. A pair the engine refuses is this call's refusal, named by its frames.
inline Result<GlobalSolution> SolveRingThreeDegreesOut(IRegistrationEngine& engine,
                                                       const std::vector<FeatureSet>& sets,
                                                       const std::vector<SyntheticFrame>& frames,
                                                       const Intrinsics& lens) {
  if (sets.size() != frames.size() || sets.empty()) {
    return Err<GlobalSolution>(StatusCode::InvalidArgument, "solved_ring",
                               "one feature set a frame, and at least one frame");
  }
  std::vector<Quat> truth;
  std::vector<FrameId> ids;
  for (const SyntheticFrame& frame : frames) {
    truth.push_back(frame.trueRotation);
    ids.push_back(frame.frame.id);
  }
  std::vector<PairwiseResult> pairs;
  for (size_t a = 0; a < sets.size(); ++a) {
    const size_t b = (a + 1) % sets.size();
    const Result<PairwiseResult> pair = engine.EstimatePairwise(
        sets[a], sets[b], PairPriorThreeDegreesOut(truth[a], truth[b]), lens);
    if (!pair.ok()) {
      return Err<GlobalSolution>(pair.status.code, "solved_ring",
                                 "pair " + std::to_string(a) + "-" + std::to_string(b) + ": " +
                                     pair.status.detail);
    }
    pairs.push_back(pair.value);
  }
  return engine.Refine(pairs, FramePriorsThreeDegreesOut(ids, truth), lens);
}

// How far the gauge the solve landed in (`score.alignment`, from scoring it against `frames`'
// truth) is from the one the frames' priors agree on, in degrees; infinite if the priors cannot be
// aligned.
inline double FacingAwayFromThePriorsDeg(const std::vector<SyntheticFrame>& frames,
                                         const RotationScore& score) {
  std::vector<Quat> truth;
  std::vector<FrameId> ids;
  for (const SyntheticFrame& frame : frames) {
    truth.push_back(frame.trueRotation);
    ids.push_back(frame.frame.id);
  }
  std::vector<Quat> orientations;
  for (const FramePrior& prior : FramePriorsThreeDegreesOut(ids, truth)) {
    orientations.push_back(prior.pose.orientation);
  }
  const GaugeAlignment agreed = BestGaugeAlignment(orientations, truth);
  if (!agreed.valid) return std::numeric_limits<double>::infinity();
  return AngleBetween(agreed.rotation, score.alignment) * 180.0 / std::numbers::pi;
}

}  // namespace sphanorama::test
