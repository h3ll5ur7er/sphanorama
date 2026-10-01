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

  // Only an absent title is an absent project; any other failure is the store's, and says so.
  if (auto title = projects_.ReadDocument(project, "title"); !title.ok()) {
    if (title.status.code != StatusCode::NotFound) return title.status;
    return Err<BuildId>(StatusCode::NotFound, kComponent,
                        Named("project", project.value) + " does not exist");
  }
  auto text = projects_.ReadDocument(project, kSessionDocumentKey);
  if (!text.ok()) {
    if (text.status.code == StatusCode::NotFound) {
      return Err<BuildId>(StatusCode::NotFound, kComponent,
                          Named("project", project.value) + " holds no capture");
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
  // Zero is a store with no spill tier, whose frames cannot outlive the tab — so a document it
  // wrote names identities that restart with every tab, and a stale one matches a live capture's
  // frames by id alone. There is nothing here that can tell the two apart; `Resume` is refused on
  // such a store by `Adopt`, and a build is refused here, rather than made from another capture's
  // pixels.
  if (generation == 0) {
    return Err<BuildId>(StatusCode::FailedPrecondition, kComponent,
                        "a device with no spill tier cannot say whose frames a capture names");
  }
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
    // By rank, the best frame whose pose was measured: one that was not is paired with nothing and
    // dropped by the solve, and choosing it would leave the cell out of a build that still
    // completes. A cell with no measured frame gives its best all the same, and loses it. The review
    // strip marks the same frame (`shell/src/clients/review/candidates.ts`); the two move together.
    const auto measured = std::find_if(candidates.begin(), candidates.end(),
                                       [](const Candidate* c) { return c->pose.confidence > 0.0; });
    const Candidate* chosen = measured != candidates.end() ? *measured : candidates.front();
    auto pick = projects_.ReadDocument(project, SelectionDocumentKey(NodeId{node}));
    if (pick.ok()) {
      const std::optional<CandidateId> named = ParseSelectionDocument(pick.value);
      if (!named) {
        return Err<BuildId>(StatusCode::Internal, kComponent,
                            "the pick recorded for " + Named("cell", node)
                                + " is not a candidate identity");
      }
      // A pick the cell no longer holds gives way to the ranking: a discarding retake took it, and
      // no screen can show a frame that is gone. An offered frame is never written down (its
      // bytes are not in the tier), so a pick of one gives way too — the gap ADR 0070 records.
      // Unless the document never saw it: a pick at or past the counter it recorded names a frame
      // it does not know — one an earlier build's door recorded, which checked nothing, or an
      // edited one; `SetSelection` takes no such pick now — and the ranking would then build from a
      // frame other than the one the pick names and the strip shows. A pick a retake discarded is
      // always below.
      if (named->value >= document.nextCandidate) {
        return Err<BuildId>(StatusCode::FailedPrecondition, kComponent,
                            "the pick recorded for " + Named("cell", node)
                                + " is newer than this capture's document");
      }
      const auto found = std::find_if(candidates.begin(), candidates.end(),
                                      [&](const Candidate* c) { return c->id == *named; });
      if (found != candidates.end()) chosen = *found;
    } else if (pick.status.code != StatusCode::NotFound) {
      return pick.status;
    }
    build.frames.push_back(chosen->frame);
    build.poses.push_back(chosen->pose);
  }

  // Two cells naming one frame would give it two priors, which the solve refuses — after every
  // extraction and pair had been paid for.
  for (size_t a = 0; a < build.frames.size(); ++a) {
    for (size_t b = a + 1; b < build.frames.size(); ++b) {
      if (build.frames[a].id == build.frames[b].id) {
        return Err<BuildId>(StatusCode::FailedPrecondition, kComponent,
                            "this capture names " + Named("frame", build.frames[a].id.value)
                                + " in two cells");
      }
    }
  }
  // Nor does the solve face a panorama nothing anchored, and it would say so only at the end.
  if (std::none_of(build.poses.begin(), build.poses.end(),
                   [](const PoseSample& pose) { return pose.confidence > 0.0; })) {
    return Err<BuildId>(StatusCode::FailedPrecondition, kComponent,
                        "no frame of this capture was taken at a measured pose");
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
      // A store that cannot answer has not said the frame is gone.
      if (residency.status.code != StatusCode::NotFound) return residency.status;
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

  // Cells overlap where their frames look within the narrower field of view of each other — by
  // the direction each looked, so a frame rolled against its neighbour still pairs with it. Only
  // measured poses are read: at confidence zero the orientation is not one (`PoseSampleDefect`),
  // and the direction `Normalize` would invent for it is straight ahead.
  const double reach = std::min(document.spec.horizontalFovDeg, document.spec.verticalFovDeg)
                       * std::numbers::pi / 180.0;
  for (size_t a = 0; a < build.frames.size(); ++a) {
    if (build.poses[a].confidence <= 0.0) continue;
    for (size_t b = a + 1; b < build.frames.size(); ++b) {
      if (build.poses[b].confidence <= 0.0) continue;
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
  if (features.ok()) build.features.push_back(features.value);
  // Extraction leaves the frame resident whatever tier it was in (`FeatureSet`), and a sphere of
  // faulted-in frames is the heap refusal cooling exists to avoid (ADR 0023). On a refusal too: an
  // engine reads the frame before it allocates its answer, so a want of room arrives with the frame
  // already faulted in, and leaving it there is the one thing that makes the want worse.
  Status restored = PutBackOrKeep(build, frame, found);
  if (!features.ok()) {
    if (!restored.ok()) features.status.detail += "; and " + restored.detail;
    return features.status;
  }
  return restored;
}

Status PanoramaBuildManager::PutBack(Misplaced& misplaced) {
  // `NotFound` is a store a new capture emptied (ADR 0034), with nowhere left to put anything back.
  auto now = frames_.ResidencyOf(misplaced.frame);
  if (now.status.code == StatusCode::NotFound) return Status::Ok();
  if (!now.ok()) return now.status;
  if (misplaced.pinOwed && now.value == Residency::HeapPinned) {
    // Pinned now and not when found: a call that could not release it left it so (`RenderPreview`
    // says as much). Nothing in the tree holds a pin across calls, so the pin is the engine's.
    if (Status released = frames_.Release(misplaced.frame); !released.ok()) return released;
    misplaced.pinOwed = false;
    // Asked again rather than assumed: pins are counted, and a holder's keeps the frame pinned.
    SPH_TRY(now.value, frames_.ResidencyOf(misplaced.frame));
  }
  misplaced.pinOwed = false;
  // The build only faults frames in, and a frame it faulted in is left unpinned in the heap. Found
  // anywhere else, it is where it was found, or where something that is not the build has put it
  // since — cooled by the capture, or pinned by a holder `Pin` made a promise to. And a frame found
  // in the heap has no colder tier to go back to.
  const bool foundInHeap =
      misplaced.found == Residency::HeapEncoded || misplaced.found == Residency::HeapPinned;
  if (now.value != Residency::HeapEncoded || foundInHeap) return Status::Ok();
  return frames_.Demote(misplaced.frame, misplaced.found);
}

Status PanoramaBuildManager::PutBackOrKeep(Build& build, const FrameRef& frame, Residency found) {
  // A frame found pinned has no pin the build could owe: every pin on it is somebody else's.
  Misplaced misplaced{frame, found, found != Residency::HeapPinned};
  Status restored = PutBack(misplaced);
  // Kept for `Cancel` and the next `Start` to retry: a capture frame left pinned is one the next
  // capture's `Clear` refuses to empty the store around.
  if (!restored.ok()) build.misplaced.push_back(misplaced);
  return restored;
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
  // Asked before the call, so a refusal that leaves a frame out of place can be put right
  // (`RenderPreview`): the frames are the capture's, not the build's. A tier that cannot be asked
  // is a frame that could not be, so the call is not made.
  std::vector<Residency> tiers;
  tiers.reserve(frames.size());
  for (const FrameRef& frame : frames) {
    SPH_TRY(const Residency tier, frames_.ResidencyOf(frame));
    tiers.push_back(tier);
  }
  auto preview = composition_.RenderPreview(build.solution, frames, GainMap{}, build.previewWidth);
  if (!preview.ok()) {
    for (size_t i = 0; i < frames.size(); ++i) {
      if (Status restored = PutBackOrKeep(build, frames[i], tiers[i]); !restored.ok()) {
        preview.status.detail += "; and " + restored.detail;
      }
    }
    // A refusal that could not give the answer back hands it over instead, and it is this build's
    // to release and forget (`RenderPreview`) — held as the preview, so a refused release is
    // retried by `Cancel` like any other.
    if (preview.value.id.valid()) {
      build.preview = preview.value;
      (void)GiveBackPreview(build);
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
    const auto discard = [this](const FrameRef& frame) {
      return frame.id.valid() ? Discard(frame) : Status::Ok();
    };
    Status descriptors = discard(set.descriptors);
    Status keypoints = discard(set.keypoints);
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

Status PanoramaBuildManager::GiveBackPreview(Build& build) {
  if (!build.preview) return Status::Ok();
  if (Status discarded = Discard(*build.preview); !discarded.ok()) return discarded;
  build.preview.reset();
  return Status::Ok();
}

Status PanoramaBuildManager::Discard(const FrameRef& frame) {
  // `NotFound` is not a refusal to let go: the store emptied itself under the build (ADR 0034), and
  // nothing named by that id will ever be forgettable again, so keeping the handle to retry would
  // strand the build rather than recover it.
  auto residency = frames_.ResidencyOf(frame);
  if (residency.status.code == StatusCode::NotFound) return Status::Ok();
  if (!residency.ok()) return residency.status;
  // The build's frames have handles nothing else was given, so every pin on one is an engine's that
  // could not release it — a preview handed back unreleased, or a feature frame behind a refused
  // `Release` the engine discarded — and the build is the only thing left that can name it.
  // One pin per call that borrowed it: a feature frame is borrowed by every pair it is in. Each
  // release takes one, so this ends.
  while (residency.value == Residency::HeapPinned) {
    if (Status released = frames_.Release(frame); !released.ok()) return released;
    SPH_TRY(residency.value, frames_.ResidencyOf(frame));
  }
  return frames_.Forget(frame);
}

Status PanoramaBuildManager::Release(Build& build) {
  Status first = ForgetFeatures(build);
  if (Status given = GiveBackPreview(build); !given.ok() && first.ok()) first = given;
  std::vector<Misplaced> still;
  for (Misplaced& misplaced : build.misplaced) {
    if (Status restored = PutBack(misplaced); !restored.ok()) {
      still.push_back(misplaced);
      if (first.ok()) first = restored;
    }
  }
  build.misplaced = std::move(still);
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
  // Asked rather than assumed: a new capture empties the store, and a handle it no longer holds is
  // not a panorama. Only `NotFound` says so; a store that could not answer has not said it is gone.
  if (auto residency = frames_.ResidencyOf(*build->preview); !residency.ok()) {
    if (residency.status.code != StatusCode::NotFound) return residency.status;
    return Err<PanoramaRef>(StatusCode::FailedPrecondition, kComponent,
                            "the store no longer holds this build's panorama; build again");
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
