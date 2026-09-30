/**
 * **The registration table, measured in the browser's build.** The same ring, the same solve and
 * the same bounds as `TheRingSolvedWithItsClosingPairIsWithinTheStatedBound`, compiled for
 * WebAssembly with the WebAssembly OpenCV and run under node (ADR 0069). A native figure says what
 * the algorithm can do; this says what the module a phone downloads does, which is the one the
 * exit criterion is about and the one a SIMD path or an exception model could make differ.
 *
 * A program rather than a GoogleTest binary, because none is built for WebAssembly, and handed a
 * rendered dataset rather than rendering one: there is no shell to run the generator from here.
 * `tools/gate.sh` and CI render it, run this under node for both WebAssembly builds, and count its
 * `[wasm-solved]` lines against `--detectors`, so a run that measured nothing is a failure.
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "engines/registration_engine/feature_registration_engine.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "support/photograph.h"
#include "support/rotation_scoring.h"
#include "support/solved_ring.h"
#include "support/synthetic_dataset.h"

using namespace sphanorama;

namespace {

// The exception boundary ADR 0052 built, under WebAssembly's exceptions rather than native ones: a
// one-pixel frame makes ORB throw out of `resize` and AKAZE out of `setSize`, and the engine has to
// hand back OpenCV's own text as a refusal, where a failed conversion aborts the module. SIFT finds
// nothing and does not throw. The same expectations as `AnswersADegenerateFrameRather...` natively.
bool ConvertsOpenCvsThrows(MemoryFrameStoreAccess& store, FeatureDetector detector) {
  const int index = static_cast<int>(detector);
  const Result<FrameRef> onePixel = store.Allocate(1, 1, PixelFormat::RGBA8);
  if (!onePixel.ok()) {
    std::printf("FAIL detector=%d: could not allocate the one-pixel frame\n", index);
    return false;
  }
  FeatureRegistrationEngine engine{store, detector};
  const Result<FeatureSet> features = engine.ExtractFeatures(onePixel.value);
  bool held;
  if (detector == FeatureDetector::Sift) {
    held = features.ok() && features.value.count == 0;
  } else {
    held = !features.ok() && features.status.code == StatusCode::Internal &&
           features.status.detail.find("OpenCV") != std::string::npos;
  }
  if (!held) {
    std::printf("FAIL detector=%d: a one-pixel frame answered %s: %s\n", index,
                features.ok() ? "ok" : "a refusal", features.status.detail.c_str());
  }
  if (!store.Forget(onePixel.value).ok()) {
    std::printf("FAIL detector=%d: the one-pixel frame was left pinned\n", index);
    held = false;
  }
  return held;
}

// One detector's solve and whether it held. Every frame and feature set is forgotten on the way out,
// and the store is asked whether that left it empty, because a leak here is otherwise silent.
bool Measure(MemoryFrameStoreAccess& store, const SyntheticDataset& dataset,
             FeatureDetector detector) {
  FeatureRegistrationEngine engine{store, detector};
  const int index = static_cast<int>(detector);
  std::vector<FeatureSet> sets;
  bool held = true;
  for (const SyntheticFrame& frame : dataset.frames) {
    const Result<FeatureSet> features = engine.ExtractFeatures(frame.frame);
    if (!features.ok()) {
      std::printf("FAIL detector=%d extract: %s\n", index, features.status.detail.c_str());
      held = false;
      break;
    }
    sets.push_back(features.value);
  }
  if (held) {
    const Result<GlobalSolution> solved =
        test::SolveRingThreeDegreesOut(engine, sets, dataset.frames, dataset.lens);
    if (!solved.ok()) {
      std::printf("FAIL detector=%d solve: %s\n", index, solved.status.detail.c_str());
      held = false;
    } else {
      std::vector<Quat> truth;
      for (const SyntheticFrame& frame : dataset.frames) truth.push_back(frame.trueRotation);
      const test::RotationScore score = test::ScoreRotations(solved.value.rotations, truth);
      std::printf("[wasm-solved] detector=%d edges=%d median=%.4f deg mean=%.4f max=%.4f "
                  "fitted=%d fx=%.3f\n",
                  index, solved.value.edgesUsed, score.medianDeg, score.meanDeg, score.maxDeg,
                  solved.value.lensFitted ? 1 : 0, solved.value.intrinsics.fx);
      const struct {
        bool ok;
        const char* what;
      } checks[] = {
          {score.valid && score.alignmentIsUnique, "the scorer could not align the solve"},
          {solved.value.converged, "the solve did not converge"},
          {solved.value.lensFitted, "the focal length was not fitted"},
          {solved.value.edgesUsed == static_cast<int32_t>(dataset.frames.size()),
           "not every pair was accepted"},
          {score.medianDeg < test::kSolvedRingMedianBoundDeg, "the median is past its bound"},
          {score.maxDeg < test::kSolvedRingMaxBoundDeg, "the worst frame is past its bound"},
          {test::FacingAwayFromThePriorsDeg(dataset.frames, score) <
               test::kSolvedRingFacingBoundDeg,
           "the solve does not face where the priors agree"},
      };
      for (const auto& check : checks) {
        if (!check.ok) {
          std::printf("FAIL detector=%d: %s\n", index, check.what);
          held = false;
        }
      }
    }
  }
  for (const FeatureSet& set : sets) {
    if (set.count == 0) continue;
    if (!store.Forget(set.descriptors).ok() || !store.Forget(set.keypoints).ok()) {
      std::printf("FAIL detector=%d: the store would not forget a feature set\n", index);
      held = false;
    }
  }
  return held;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--detectors") == 0) {
    std::printf("%d\n", static_cast<int>(kAllFeatureDetectors.size()));
    return 0;
  }
  // The ring to render and the world to render it from, from the one place each is written, so
  // the script holds no copy to drift.
  if (argc == 2 && std::strcmp(argv[1], "--ring") == 0) {
    std::printf("%d %d %d %s\n", test::kSolvedRingFrames, test::kSolvedRingWidth,
                test::kSolvedRingHeight, test::kPhotograph);
    return 0;
  }
  if (argc != 2) {
    std::printf("usage: %s <rendered dataset directory> | --detectors | --ring\n", argv[0]);
    return 2;
  }
  MemoryFrameStoreAccess store{1 << 28};
  const Result<SyntheticDataset> dataset = LoadSyntheticDataset(store, argv[1]);
  if (!dataset.ok()) {
    std::printf("FAIL load: %s\n", dataset.status.detail.c_str());
    return 1;
  }
  // A ring of another shape is a different measurement from the one the bounds were set on.
  bool held = dataset.value.frames.size() == static_cast<size_t>(test::kSolvedRingFrames) &&
              dataset.value.lens.width == test::kSolvedRingWidth &&
              dataset.value.lens.height == test::kSolvedRingHeight;
  if (!held) {
    std::printf("FAIL: the dataset is %zu frames of %d by %d, and the ring is %d of %d by %d\n",
                dataset.value.frames.size(), dataset.value.lens.width, dataset.value.lens.height,
                test::kSolvedRingFrames, test::kSolvedRingWidth, test::kSolvedRingHeight);
  }
  for (const FeatureDetector detector : kAllFeatureDetectors) {
    held = ConvertsOpenCvsThrows(store, detector) && held;
    held = Measure(store, dataset.value, detector) && held;
  }
  for (const SyntheticFrame& frame : dataset.value.frames) {
    if (!store.Forget(frame.frame).ok()) {
      std::printf("FAIL: the store would not forget a frame\n");
      held = false;
    }
  }
  const Result<FrameStoreBudget> budget = store.Budget();
  if (!budget.ok() || budget.value.heapUsedBytes != 0) {
    std::printf("FAIL: the store did not end empty\n");
    held = false;
  }
  return held ? 0 : 1;
}
