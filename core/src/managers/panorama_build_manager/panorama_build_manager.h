#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include "sphanorama/engines/composition_engine.h"
#include "sphanorama/engines/registration_engine.h"
#include "sphanorama/managers/panorama_build_manager.h"
#include "sphanorama/resource_access/frame_store_access.h"
#include "sphanorama/resource_access/project_store_access.h"

namespace sphanorama {

// Owns a build: reads what a capture wrote down, and sequences registration and composition over
// it one step per `Poll` (ADR 0070).
class PanoramaBuildManager final : public IPanoramaBuildManager {
 public:
  PanoramaBuildManager(IRegistrationEngine& registration, ICompositionEngine& composition,
                       IFrameStoreAccess& frames, IProjectStoreAccess& projects);

  Result<BuildId> Start(ProjectId project, const BuildSpec& spec) override;
  Result<BuildProgress> Poll(BuildId build) override;
  Result<PanoramaRef> Panorama(BuildId build) override;
  Result<FramePreview> PanoramaPreview(BuildId build) override;
  Result<GhostReport> Ghosts(BuildId build) override;
  Status Invalidate(BuildId build, std::span<const NodeId> dirty) override;
  Status Cancel(BuildId build) override;

 private:
  // A capture frame the build handed to an engine and has not yet put back where it was found.
  struct Misplaced {
    FrameRef frame;
    Residency found = Residency::HeapEncoded;
    // Whether a pin the frame carries may be an engine's that could not release it, which the build
    // then owes. Once released, a pin is somebody else's — `Pin` promises them their mapping.
    bool pinOwed = true;
  };

  struct Build {
    BuildProgress progress;
    Intrinsics lens;
    int32_t previewWidth = 0;
    // One per cell, in the order of their node ids, with the pose each was taken at.
    std::vector<FrameRef> frames;
    std::vector<PoseSample> poses;
    // Indices into `frames`.
    std::vector<std::pair<size_t, size_t>> pairs;
    // Filled one frame per step, and forgotten once the last pair is estimated.
    std::vector<FeatureSet> features;
    std::vector<PairwiseResult> estimated;
    GlobalSolution solution;
    std::optional<FrameRef> preview;
    // Capture frames a refusal left out of their tier that the store would not put back yet.
    std::vector<Misplaced> misplaced;
    size_t done = 0;
  };

  size_t StepCount(const Build& build) const;
  Status Step(Build& build);
  Status ExtractFeatures(Build& build, size_t frame);
  // Back to the tier it was found in, since reading it faulted it in. Records how far it got.
  Status PutBack(Misplaced& frame);
  Status PutBackOrKeep(Build& build, const FrameRef& frame, Residency found);
  Status EstimatePair(Build& build, size_t pair);
  Status Solve(Build& build);
  Status Compose(Build& build);
  // Everything the build holds, given back. Answers the first refusal and keeps what was refused.
  Status Release(Build& build);
  Status ForgetFeatures(Build& build);
  Status GiveBackPreview(Build& build);
  // Releases and forgets a frame the build made. Ok for one the store no longer holds.
  Status Discard(const FrameRef& frame);
  void Abandon(Build& build, Status why);
  void Report(Build& build);
  Result<Build*> Find(BuildId id);

  IRegistrationEngine& registration_;
  ICompositionEngine& composition_;
  IFrameStoreAccess& frames_;
  IProjectStoreAccess& projects_;
  std::optional<Build> build_;
  uint64_t next_build_ = 1;
};

}  // namespace sphanorama
