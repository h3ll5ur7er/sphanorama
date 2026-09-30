#include "managers/panorama_build_manager/panorama_build_manager.h"

#include <algorithm>
#include <map>
#include <numbers>
#include <string>

#include "utilities/camera_model.h"
#include "utilities/quaternion.h"
#include "utilities/session_document.h"

namespace sphanorama {
namespace {
constexpr const char* kComponent = "PanoramaBuildManager";

// The widest preview a build draws, whatever the spec's output: 2048 by 1024 is 8 MB of RGBA, which
// a phone's store holds beside a frame being read, and wider than a phone's screen.
constexpr int32_t kWidestPreview = 2048;

std::string Named(const char* what, uint64_t id) {
  return std::string(what) + " " + std::to_string(id);
}
}  // namespace

PanoramaBuildManager::PanoramaBuildManager(IRegistrationEngine& registration,
                                           ICompositionEngine& composition,
                                           IFrameStoreAccess& frames,
                                           IProjectStoreAccess& projects)
    : registration_(registration), composition_(composition), frames_(frames),
      projects_(projects) {}

Result<BuildId> PanoramaBuildManager::Start(ProjectId project, const BuildSpec& spec) {
  if (spec.projection != Projection::Equirectangular) {
    return Err<BuildId>(StatusCode::Unsupported, kComponent, "only an equirectangular panorama");
  }
  if (spec.outputWidth < 2) {
    return Err<BuildId>(StatusCode::InvalidArgument, kComponent,
                        "a panorama narrower than two pixels has no pixel to draw");
  }
  if (build_ && build_->progress.stage != BuildStage::Complete
      && build_->progress.stage != BuildStage::Failed) {
    return Err<BuildId>(StatusCode::FailedPrecondition, kComponent,
                        "a build is already running; cancel it first");
  }

  if (!projects_.ReadDocument(project, "title").ok()) {
    return Err<BuildId>(StatusCode::NotFound, kComponent, "no such project");
  }
  auto text = projects_.ReadDocument(project, kSessionDocumentKey);
  if (!text.ok()) {
    if (text.status.code == StatusCode::NotFound) {
      return Err<BuildId>(StatusCode::NotFound, kComponent, "this project holds no capture");
    }
    return text.status;
  }
  SessionDocument document;
  if (!DecodeSessionDocument(text.value, document)) {
    return Err<BuildId>(StatusCode::Unsupported, kComponent,
                        "this project's capture was written by a build that cannot be read here");
  }

  // The tier first, because it decides whether the identities below name this capture's pixels at
  // all (ADR 0035).
  SPH_TRY(const uint64_t generation, frames_.TierGeneration());
  if (generation != document.generation) {
    return Err<BuildId>(StatusCode::FailedPrecondition, kComponent,
                        "the frames this capture names belong to another capture's tier");
  }

  // Cells in node order, each cell's candidates in the order the document lists them — the order
  // the ranking left them in, best first.
  std::map<uint64_t, std::vector<const Candidate*>> cells;
  for (const Candidate& candidate : document.candidates) {
    cells[candidate.node.value].push_back(&candidate);
  }
  if (cells.empty()) {
    return Err<BuildId>(StatusCode::FailedPrecondition, kComponent, "nothing has been captured");
  }

  Build build;
  for (const auto& [node, candidates] : cells) {
    const Candidate* chosen = candidates.front();
    auto pick = projects_.ReadDocument(project, SelectionDocumentKey(NodeId{node}));
    if (pick.ok()) {
      const std::optional<CandidateId> named = ParseSelectionDocument(pick.value);
      if (!named) {
        return Err<BuildId>(StatusCode::Internal, kComponent,
                            "the pick recorded for " + Named("cell", node)
                                + " is not a candidate identity");
      }
      const auto found = std::find_if(candidates.begin(), candidates.end(),
                                      [&](const Candidate* c) { return c->id == *named; });
      if (found == candidates.end()) {
        return Err<BuildId>(StatusCode::FailedPrecondition, kComponent,
                            "the pick recorded for " + Named("cell", node) + " names "
                                + Named("candidate", named->value)
                                + ", which the cell does not hold");
      }
      chosen = *found;
    } else if (pick.status.code != StatusCode::NotFound) {
      return pick.status;
    }
    build.frames.push_back(chosen->frame);
    build.poses.push_back(chosen->pose);
  }

  const int32_t width = build.frames.front().width;
  const int32_t height = build.frames.front().height;
  for (const FrameRef& frame : build.frames) {
    if (frame.width != width || frame.height != height) {
      return Err<BuildId>(StatusCode::FailedPrecondition, kComponent,
                          "this capture's frames are not all one size");
    }
    // Asked rather than assumed: a tab reloaded without resuming has a store that never held them.
    if (auto residency = frames_.ResidencyOf(frame); !residency.ok()) {
      return Err<BuildId>(StatusCode::FailedPrecondition, kComponent,
                          "the store does not hold " + Named("frame", frame.id.value)
                              + " of this capture: " + residency.status.detail);
    }
  }

  build.lens = LensFromFieldOfView(document.spec.horizontalFovDeg, document.spec.verticalFovDeg,
                                   width, height);
  if (!IsUsableLens(build.lens)) {
    return Err<BuildId>(StatusCode::FailedPrecondition, kComponent,
                        "this capture's field of view is not a lens");
  }

  // Cells overlap where their frames look within the narrower field of view of each other.
  const double reach = std::min(document.spec.horizontalFovDeg, document.spec.verticalFovDeg)
                       * std::numbers::pi / 180.0;
  for (size_t a = 0; a < build.frames.size(); ++a) {
    for (size_t b = a + 1; b < build.frames.size(); ++b) {
      const double apart = AngleBetweenDirections(Direction(build.poses[a].orientation),
                                                  Direction(build.poses[b].orientation));
      if (apart < reach) build.pairs.emplace_back(a, b);
    }
  }
  build.previewWidth = std::min(spec.outputWidth, kWidestPreview);

  // Last, since everything above can refuse and the finished build stands until this succeeds.
  if (build_) {
    if (Status released = Release(*build_); !released.ok()) return released;
    build_.reset();
  }
  build.progress.id = BuildId{next_build_++};
  build.progress.stage = BuildStage::Queued;
  build_ = std::move(build);
  return Ok(build_->progress.id);
}

size_t PanoramaBuildManager::StepCount(const Build& build) const {
  return build.frames.size() + build.pairs.size() + 2;
}

Result<BuildProgress> PanoramaBuildManager::Poll(BuildId id) {
  SPH_TRY(Build* const build, Find(id));
  const BuildStage stage = build->progress.stage;
  if (stage == BuildStage::Complete || stage == BuildStage::Failed) return Ok(build->progress);
  if (Status stepped = Step(*build); !stepped.ok()) {
    Abandon(*build, std::move(stepped));
  } else {
    ++build->done;
    Report(*build);
  }
  return Ok(build->progress);
}

Status PanoramaBuildManager::Step(Build& build) {
  const size_t frames = build.frames.size();
  const size_t pairs = build.pairs.size();
  if (build.done < frames) return ExtractFeatures(build, build.done);
  if (build.done < frames + pairs) return EstimatePair(build, build.done - frames);
  if (build.done == frames + pairs) return Solve(build);
  return Compose(build);
}

Status PanoramaBuildManager::ExtractFeatures(Build& build, size_t index) {
  const FrameRef& frame = build.frames[index];
  SPH_TRY(const Residency found, frames_.ResidencyOf(frame));
  auto features = registration_.ExtractFeatures(frame);
  if (!features.ok()) return features.status;
  build.features.push_back(features.value);
  // Extraction leaves the frame resident whatever tier it was in (`FeatureSet`), and a sphere of
  // faulted-in frames is the heap refusal cooling exists to avoid (ADR 0023).
  if (found != Residency::HeapPinned) {
    SPH_TRY(const Residency now, frames_.ResidencyOf(frame));
    if (now != found) return frames_.Demote(frame, found);
  }
  return Status::Ok();
}

Status PanoramaBuildManager::EstimatePair(Build& build, size_t index) {
  const auto [a, b] = build.pairs[index];
  const FeatureSet& first = build.features[a];
  const FeatureSet& second = build.features[b];
  // A frame with nothing to match is placed by its prior; asking would be refused as malformed.
  if (first.count > 0 && second.count > 0) {
    const Quat prior = Normalize(Multiply(Conjugate(build.poses[b].orientation),
                                          build.poses[a].orientation));
    auto pair = registration_.EstimatePairwise(first, second, prior, build.lens);
    if (pair.ok()) {
      build.estimated.push_back(std::move(pair.value));
    } else if (pair.status.code != StatusCode::RegistrationFailed) {
      // A declined edge is an answer about the pixels; anything else is the build going wrong.
      return pair.status;
    }
  }
  if (index + 1 == build.pairs.size()) return ForgetFeatures(build);
  return Status::Ok();
}

Status PanoramaBuildManager::Solve(Build& build) {
  // With no pairs at all the features were never needed past their extraction.
  if (build.pairs.empty()) {
    if (Status forgotten = ForgetFeatures(build); !forgotten.ok()) return forgotten;
  }
  std::vector<FramePrior> priors;
  priors.reserve(build.frames.size());
  for (size_t i = 0; i < build.frames.size(); ++i) {
    priors.push_back(FramePrior{build.frames[i].id, build.poses[i]});
  }
  auto solved = registration_.Refine(build.estimated, priors, build.lens);
  if (!solved.ok()) return solved.status;
  build.solution = std::move(solved.value);
  build.estimated.clear();
  return Status::Ok();
}

Status PanoramaBuildManager::Compose(Build& build) {
  std::vector<FrameRef> frames;
  frames.reserve(build.solution.frames.size());
  for (const FrameId id : build.solution.frames) {
    const auto found = std::find_if(build.frames.begin(), build.frames.end(),
                                    [id](const FrameRef& frame) { return frame.id == id; });
    if (found == build.frames.end()) {
      return Fail(StatusCode::Internal, kComponent,
                  "the solve placed " + Named("frame", id.value) + ", which the build never read");
    }
    frames.push_back(*found);
  }
  auto preview = composition_.RenderPreview(build.solution, frames, GainMap{}, build.previewWidth);
  if (!preview.ok()) {
    // A refusal that could not give the answer back hands it over instead, and it is this build's
    // to release and forget (`RenderPreview`).
    if (preview.value.id.valid()) {
      auto residency = frames_.ResidencyOf(preview.value);
      if (residency.ok() && residency.value == Residency::HeapPinned) {
        (void)frames_.Release(preview.value);
      }
      if (!frames_.Forget(preview.value).ok()) build.preview = preview.value;
    }
    return preview.status;
  }
  build.preview = preview.value;
  return Status::Ok();
}

Status PanoramaBuildManager::ForgetFeatures(Build& build) {
  Status first;
  std::vector<FeatureSet> kept;
  for (const FeatureSet& set : build.features) {
    // A count of zero allocated nothing, and its handles name no frame; nor does one an earlier
    // pass already forgot.
    if (set.count == 0) continue;
    const auto forget = [this](const FrameRef& frame) {
      return frame.id.valid() ? frames_.Forget(frame) : Status::Ok();
    };
    Status descriptors = forget(set.descriptors);
    Status keypoints = forget(set.keypoints);
    if (!descriptors.ok() || !keypoints.ok()) {
      FeatureSet left = set;
      if (descriptors.ok()) left.descriptors = FrameRef{};
      if (keypoints.ok()) left.keypoints = FrameRef{};
      kept.push_back(left);
      if (first.ok()) first = descriptors.ok() ? keypoints : descriptors;
    }
  }
  build.features = std::move(kept);
  return first;
}

Status PanoramaBuildManager::Release(Build& build) {
  Status first = ForgetFeatures(build);
  if (build.preview) {
    if (Status forgotten = frames_.Forget(*build.preview); forgotten.ok()) {
      build.preview.reset();
    } else if (first.ok()) {
      first = forgotten;
    }
  }
  return first;
}

void PanoramaBuildManager::Abandon(Build& build, Status why) {
  // Given back on the way out, so a failed build holds nothing a caller has to cancel to recover.
  // What the store refused stays named here, and a `Cancel` retries it.
  (void)ForgetFeatures(build);
  build.progress.stage = BuildStage::Failed;
  build.progress.failure = std::move(why);
}

void PanoramaBuildManager::Report(Build& build) {
  const size_t frames = build.frames.size();
  const size_t pairs = build.pairs.size();
  const size_t total = StepCount(build);
  if (build.done == total) {
    build.progress.stage = BuildStage::Complete;
    build.progress.fraction = 1.0;
    return;
  }
  // Named for the step the next `Poll` does.
  if (build.done < frames) {
    build.progress.stage = BuildStage::Features;
  } else if (build.done < frames + pairs) {
    build.progress.stage = BuildStage::PairwiseMatching;
  } else if (build.done == frames + pairs) {
    build.progress.stage = BuildStage::GlobalSolve;
  } else {
    build.progress.stage = BuildStage::Projecting;
  }
  build.progress.fraction = static_cast<double>(build.done) / static_cast<double>(total);
}

Result<PanoramaBuildManager::Build*> PanoramaBuildManager::Find(BuildId id) {
  if (!build_ || build_->progress.id != id) {
    return Err<Build*>(StatusCode::NotFound, kComponent, "no such build");
  }
  return Ok(&*build_);
}

Result<PanoramaRef> PanoramaBuildManager::Panorama(BuildId id) {
  SPH_TRY(Build* const build, Find(id));
  if (build->progress.stage != BuildStage::Complete || !build->preview) {
    return Err<PanoramaRef>(StatusCode::FailedPrecondition, kComponent,
                            "this build has not completed");
  }
  PanoramaRef panorama;
  panorama.build = id;
  panorama.projection = Projection::Equirectangular;
  panorama.width = build->preview->width;
  panorama.height = build->preview->height;
  panorama.preview = *build->preview;
  return Ok(panorama);
}

Result<GhostReport> PanoramaBuildManager::Ghosts(BuildId id) {
  SPH_TRY(Build* const build, Find(id));
  (void)build;
  return Err<GhostReport>(StatusCode::Unsupported, kComponent, "nothing detects movers yet");
}

Status PanoramaBuildManager::Invalidate(BuildId id, std::span<const NodeId>) {
  SPH_TRY(Build* const build, Find(id));
  (void)build;
  return Fail(StatusCode::Unsupported, kComponent,
              "an incremental rebuild is not built yet; start a new build");
}

Status PanoramaBuildManager::Cancel(BuildId id) {
  SPH_TRY(Build* const build, Find(id));
  if (Status released = Release(*build); !released.ok()) {
    if (build->progress.stage != BuildStage::Complete) {
      build->progress.stage = BuildStage::Failed;
      build->progress.failure = Fail(StatusCode::Cancelled, kComponent, "cancelled");
    }
    return released;
  }
  build_.reset();
  return Status::Ok();
}

}  // namespace sphanorama
