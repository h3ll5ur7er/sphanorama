#pragma once
#include <string>
#include <vector>

#include "sphanorama/engines/registration_engine.h"
#include "support/synthetic_dataset.h"
#include "support/wrong_priors.h"

namespace sphanorama::test {

// The photograph ring solved with its closing pair, against priors three degrees out: the
// measurement `registration_accuracy_test.cpp` makes natively and `registration_accuracy_wasm.cpp`
// makes in the browser's build (ADR 0069). One copy, so the two are the same solve held to the same
// bounds, and a difference between them is the platform's rather than the harness's.

// Why these two, and what they are measured against: `TheRingSolvedWithItsClosingPair...`.
inline constexpr double kSolvedRingMedianBoundDeg = 0.08;
inline constexpr double kSolvedRingMaxBoundDeg = 0.25;

// Every consecutive pair of `sets`, the last with the first, estimated against the true step three
// degrees out, then refined against every frame's prior three degrees out. `sets[i]` is `frames[i]`'s.
// A pair the engine refuses is this call's refusal, named by its frames.
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

}  // namespace sphanorama::test
