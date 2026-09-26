/**
 * **How wrong are the rotations?** This is the measurement Phase 2 exits on, and until this file
 * existed nothing in the repository answered it. It answers two questions now — this suite measures
 * the error, and `Acceptance` below asserts that a refusal and a declined answer are different
 * outcomes in practice, which is the claim ADR 0056 rests on.
 *
 * The three pieces it joins were built separately and on purpose: `tools/synth_dataset.py` renders
 * frames and the truth of where the camera looked (ADR 0050), `support/synthetic_dataset` reads
 * them into a frame store (ADR 0053), and `support/rotation_scoring` says how wrong a set of
 * estimated rotations is once the arbitrary world frame is removed (ADR 0049). What was missing was
 * something that put a registration between them.
 *
 * **The dataset is generated here rather than committed**, which is the third option ADR 0053's
 * Rejected section did not weigh. The committed `synthetic-ring-4` is a *format* fixture — four
 * 48x36 frames, enough to pin the file format and far too small to measure a detector on. A
 * measurement dataset is a different thing and ADR 0053 says it must not be committed, so this
 * renders one into a temporary directory and skips when the generator is not available. A skipped
 * check is worse than a passing one and much better than a broken build on a machine without `uv`.
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

#include "engines/registration_engine/feature_registration_engine.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "support/match_noise.h"
#include "support/rendered_dataset.h"
#include "support/rotation_scoring.h"
#include "support/synthetic_dataset.h"
#include "utilities/camera_model.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

namespace fs = std::filesystem;
using test::Rendered;
using test::World;

/**
 * Forgets a frame and says so if the store refuses, which is the only signal a leaked pin gives.
 *
 * `Forget` fails while a frame is still pinned, so a discarded status is a leak nothing reports: a
 * sabotaged `~BorrowedFrame` leaking four pins a pair left this suite green with every number
 * identical to the digit, while the engine's own suite went red in twenty-one places. The pattern is
 * `ForgetOutputs`' in `registration_engine_test.cpp`; this file was the one that had not adopted
 * it.
 */
void Release(IFrameStoreAccess& store, const FrameRef& frame) {
  const Status status = store.Forget(frame);
  // The store's own sentence, not a guess at which refusal this was. The first version named a
  // surviving pin, which is the refusal this exists to catch and not the only one it can get: a
  // zero-count `FeatureSet` carries default `FrameRef`s that were never allocated, and forgetting
  // one answers `NotFound` — a message blaming a pin sends the reader looking for the wrong bug.
  EXPECT_TRUE(status.ok())
      << "the store would not forget a frame: " << status.detail
      << " — a surviving pin is the usual cause, and a handle it never allocated is the other";
}

/**
 * Nothing is left in the store, which is the half `Release` cannot see.
 *
 * `Release` catches a frame that will not go — a pin nobody dropped. It cannot catch a frame nobody
 * *asked* it about: deleting one `Release(store, set.keypoints)` strands 43 to 48 KB per detector
 * (47848, 43496 and 44296 measured here) and leaves all three parameters `[ OK ]`, every number
 * identical to the digit, process exit 0. The two questions need two instruments, and this one is
 * already in the tree — `synthetic_dataset_alloc_test.cpp` asks the budget exactly this way.
 */
/**
 * Everything the measurement allocated, given back on every path out — a failed assertion included.
 *
 * The release loops these replace sat at the end of the function body, and this file has **five**
 * `ASSERT_*` between the first allocation and them. `ASSERT_*` returns, so any one of them left the
 * whole dataset in the store — 14,745,600 bytes for twelve 640x480 frames — and skipped the very
 * check that would have said so. That is `synthetic_dataset.h`'s stated contract broken on exactly
 * the paths nobody was looking at: "**It belongs to the caller**, who must `Forget` it … a harness
 * that leaked a dataset per run would exhaust the heap somewhere in the middle of a measurement and
 * report that instead of an accuracy number."
 *
 * A destructor needs no reachability argument, which is the point: the assertions above it can grow
 * without anyone remembering this. The budget check moved inside for the same reason — at the end of
 * the body it would have run *before* this cleanup and read non-zero.
 */
class Owned {
 public:
  /** Whether this one also asserts the store ended empty — true for the outermost holder only. */
  enum class Then { kExpectEmpty, kJustGiveBack };

  explicit Owned(IFrameStoreAccess& store, Then then = Then::kExpectEmpty)
      : store_(store), then_(then) {}
  ~Owned() {
    for (const FeatureSet& set : sets) {
      // **A count of zero allocated nothing**, which `IRegistrationEngine::ExtractFeatures` states
      // and `ForgetOutputs` in `registration_engine_test.cpp` already guards. Such a set carries default
      // `FrameRef`s, and `MemoryFrameStoreAccess` numbers frames from 1, so forgetting one answers
      // `NotFound` — two spurious failures about a store that is correctly empty. Not reachable
      // from these two tests today; reachable in this repository, which is enough.
      if (set.count == 0) continue;
      Release(store_, set.descriptors);
      Release(store_, set.keypoints);
    }
    for (const FrameRef& frame : frames) Release(store_, frame);
    if (then_ == Then::kJustGiveBack) return;
    const Result<FrameStoreBudget> budget = store_.Budget();
    EXPECT_TRUE(budget.ok()) << budget.status.detail;
    if (budget.ok()) {
      EXPECT_EQ(budget.value.heapUsedBytes, 0)
          << "the measurement ended with " << budget.value.heapUsedBytes
          << " bytes still in the store, so something it allocated was never forgotten";
    }
  }
  Owned(const Owned&) = delete;
  Owned& operator=(const Owned&) = delete;

  std::vector<FrameRef> frames;
  std::vector<FeatureSet> sets;

 private:
  IFrameStoreAccess& store_;
  Then then_;
};

/**
 * The relative rotation this engine answers with, turned into the absolute one the scorer wants.
 *
 * `truth.json` records device -> world per frame, so a direction in the world reaches frame `i`'s
 * camera space as `conjugate(q_i) * w`. Two frames therefore relate by
 * `bearing_b = conjugate(q_b) * q_a * bearing_a`, which is what `EstimatePairwise` measures — so
 * `R_ab = conjugate(q_b) * q_a` and `q_b = q_a * conjugate(R_ab)`.
 *
 * Derived rather than discovered by trying both: an inverted convention scores as a large error on
 * every frame, and "try the other one and keep whichever scores better" is how a harness ends up
 * certifying the convention it was supposed to check.
 *
 * **And the test catches an inversion, for a reason that depends on `kFrames`.** The prior below is
 * built with this same convention, so inverting *both* makes the prior `conjugate(R)`. At twelve
 * frames the steps are 30 degrees, so `conjugate(R)` sits 60 degrees from `R` — outside
 * `kPriorBoundDeg = 45` — and every sampled hypothesis near the truth is rejected, so every step
 * refuses and the registered-pairs assertion fails. Inverting only one of the two leaves 60 degrees
 * of error per step and the median blows past the bound.
 *
 * That defence rides on the bound being narrower than twice the inter-frame step, which it stops
 * being at twenty-four frames: 15-degree steps put `conjugate(R)` 30 degrees away, inside the
 * bound, where a doubly-inverted convention would be absorbed by the seed. Worth knowing before
 * anyone raises `kFrames`.
 */
Quat Chain(const Quat& previousAbsolute, const Quat& relative) {
  return Normalize(Multiply(previousAbsolute, Conjugate(relative)));
}

class Accuracy : public ::testing::TestWithParam<FeatureDetector> {};

TEST_P(Accuracy, ConsecutiveFramesOfARingRegisterToWithinTheStatedBound) {
  constexpr int kFrames = 12;
  Rendered rendered(kFrames, 640, 480, World::Photograph);
  ASSERT_FALSE(rendered.inputMissing()) << rendered.why();
  if (!rendered.ok()) {
    GTEST_SKIP() << "the dataset generator did not run, so nothing was measured — and a skipped "
                    "measurement is not a passing one. "
                 << rendered.why();
  }

  MemoryFrameStoreAccess store{1 << 28};
  // Declared before anything is allocated, so it outlives every `ASSERT_*` below and unwinds last.
  Owned owned{store};
  const Result<SyntheticDataset> dataset = LoadSyntheticDataset(store, rendered.path());
  ASSERT_TRUE(dataset.ok()) << dataset.status.detail;
  for (const SyntheticFrame& frame : dataset.value.frames) owned.frames.push_back(frame.frame);
  ASSERT_EQ(dataset.value.frames.size(), static_cast<size_t>(kFrames));

  FeatureRegistrationEngine engine{store, GetParam()};

  std::vector<FeatureSet>& sets = owned.sets;
  for (const SyntheticFrame& frame : dataset.value.frames) {
    const Result<FeatureSet> features = engine.ExtractFeatures(frame.frame);
    ASSERT_TRUE(features.ok()) << features.status.detail;
    sets.push_back(features.value);
  }

  // Anchored at the first frame's truth, so the chain measures the *rotations between* frames and
  // not the arbitrary choice of where to start. The gauge removal in `ScoreRotations` would absorb a
  // different anchor anyway; starting from truth keeps the two independent.
  std::vector<Quat> estimated{dataset.value.frames.front().trueRotation};
  std::vector<Quat> truth{dataset.value.frames.front().trueRotation};
  int unregistered = 0;
  for (size_t at = 1; at < sets.size(); ++at) {
    // **The prior is perturbed, and that is the difference between a measurement and a mirror.**
    // The first version passed the exact truth of the step — and `FitRotation` seeds its search with
    // the prior, so the answer was already correct before a single pixel was read. A reviewer gutted
    // the estimator to `return the prior` and all three detectors passed with `median = 0.0000`,
    // scoring *better* than the real implementation; in 31 of 33 steps RANSAC never beat the prior's
    // inlier count. The numbers that came out of that arrangement were published in the roadmap and
    // were an artefact.
    //
    // Three degrees, about an axis that is not the one the ring turns about, so the perturbation
    // cannot be absorbed by the very rotation being estimated. That is the order a fused phone
    // orientation is out by when it is working, so a registration that cannot beat it is not worth
    // having — and one that can is being measured on the pixels, which is the point.
    const Quat truthStep = Multiply(Conjugate(dataset.value.frames[at].trueRotation),
                                    dataset.value.frames[at - 1].trueRotation);
    const Quat nudge = FromAxisAngle(Vec3{1, 0, 0}, 3.0 * std::numbers::pi / 180.0);
    const Quat prior = Normalize(Multiply(truthStep, nudge));
    const Result<PairwiseResult> pair =
        engine.EstimatePairwise(sets[at - 1], sets[at], prior, dataset.value.lens);
    if (!pair.ok() || !pair.value.accepted) {
      ++unregistered;
      // **A step with no accepted answer carries truth forward, and that flatters the score** — a
      // detector that declined every step would chain pure truth and read as perfect. Which is why
      // the count below is a conjunct of this test and not a line in its output: the accuracy number
      // means nothing without the share of the ring it was computed over.
      estimated.push_back(dataset.value.frames[at].trueRotation);
      truth.push_back(dataset.value.frames[at].trueRotation);
      continue;
    }
    // The matches carried are the inliers counted, each within the engine's 3-pixel inlier gate of
    // where the returned rotation puts it — so a `Refine` refitting from them refits this pair and
    // not some other set (ADR 0066). The thousandth covers the matches being floats.
    EXPECT_EQ(pair.value.inlierMatches.size(), static_cast<size_t>(pair.value.inliers)) << at;
    for (const PixelMatch& match : pair.value.inlierMatches) {
      const UnprojectedDirection from = Unproject(dataset.value.lens, Pixel{match.ax, match.ay});
      ASSERT_TRUE(from.valid) << at;
      const ProjectedPixel landed =
          Project(dataset.value.lens, Rotate(pair.value.relativeRotation, from.direction));
      ASSERT_TRUE(landed.valid) << at;
      EXPECT_LE(std::hypot(landed.pixel.x - match.bx, landed.pixel.y - match.by), 3.001) << at;
    }
    estimated.push_back(Chain(estimated.back(), pair.value.relativeRotation));
    truth.push_back(dataset.value.frames[at].trueRotation);
  }

  const test::RotationScore score = test::ScoreRotations(estimated, truth);
  ASSERT_TRUE(score.valid) << "the scorer could not align the two sets";
  // **A precondition, not a check on the estimator — and it cannot fail on this dataset.** `maxDeg`
  // and `medianDeg` are residuals measured after a common rotation is divided out, so if that
  // rotation were not unique they would describe an arbitrary choice among several. On a twelve
  // frame ring the eigen-gap is nowhere near degenerate and this is always true; a reviewer
  // measured the relative gap at 1e-12 from the threshold and was right to say the assertion is
  // decoration today.
  //
  // Kept anyway, and labelled, for the same reason as `ASSERT_TRUE(score.valid)` above it: the
  // degenerate case is reachable in life — `rotation_scoring.h` names two frames exactly a half
  // turn apart — so someone changing `kFrames` or the ring geometry into that shape should get a
  // failure here rather than numbers below that have quietly stopped meaning anything. An assertion
  // that guards a precondition is allowed not to fire; what it must not do is read as evidence
  // about the thing under test, which is why this comment says which it is.
  ASSERT_TRUE(score.alignmentIsUnique)
      << "the gauge alignment is degenerate, so the residual below is one of several answers";

  const int steps = kFrames - 1;
  std::fprintf(
      stderr,
      "[accuracy] detector=%d frames=%d registered=%d/%d median=%.4f deg mean=%.4f max=%.4f\n",
      static_cast<int>(GetParam()), kFrames, steps - unregistered, steps, score.medianDeg,
      score.meanDeg, score.maxDeg);

  // **The anti-echo floor is gone, and the reason is worth keeping.** A `medianDeg < 3.0` assertion
  // stood here, defended on the grounds that it is tied to the three-degree `nudge` while the
  // accuracy bound is tied to the detectors. That was arguable at a 0.5 bound and is not at 0.2: it
  // is strictly subsumed, so it can never be the assertion that fails, and a test that credits it
  // with catching the prior echo is miscrediting it. What actually catches an echo is the
  // perturbation itself — a returned prior wins no inliers at three degrees, so the chain refuses
  // and the registered-pairs conjunct below fails. Verified by sabotage rather than argued.

  // **Every pair, not most of them — which is what makes `World::Photograph` load-bearing.** This
  // asked for more than half until a reviewer set the world to `Checkerboard` and watched all three
  // detectors stay green on the pre-branch numbers: the bounds below were calibrated in the harder
  // world, so the easier one cannot fail them and the world the measurement is taken in was a
  // comment. The checkerboard leaves ORB at 8 of 11 and the hangar gives 11 of 11 to all three, so
  // an equality here fails loudly if `--panorama` ever stops arriving.
  //
  // **It rests on ORB, and on two things outside this file.** Every other assertion here passes in
  // both worlds for all three detectors, so this conjunct on this one parameter is the whole of the
  // discrimination — and the checkerboard is a world this suite never renders, so a better matcher
  // that registered all eleven there would take it away silently. What holds the rest of the chain:
  // `Rendered` fails rather than skips when the panorama is not in the tree, and
  // `tools/asset_provenance.py` and `TheCommittedPanoramaIsWhatItsRecordSays` pin the file at that
  // path to the bytes and the shape recorded for it. The world is held by three things together;
  // none of them alone is enough.
  EXPECT_EQ(steps - unregistered, steps)
      << "only " << (steps - unregistered) << " of " << steps
      << " consecutive pairs produced an accepted rotation";

  // **0.5 degrees, and it is a bound rather than a target.** An earlier version of this comment
  // published per-detector medians here and in the roadmap; those came from the arrangement that
  // handed the estimator the exact truth as its prior, so they measured the prior. Under a
  // perturbed prior the medians are in the same neighbourhood but they are now a measurement, and
  // the bound is deliberately several times looser than any of them — per the skill's advice that a
  // bound which exists beats a precise one that does not. It is still far tighter than the
  // 175-to-179-degree aliases this dataset produced before the prior was bounded.
  // **A regression bound, not the phase threshold** — and the distinction is what a reviewer had to
  // point out. `docs/06-roadmap.md` states 0.5 degrees as what Phase 2 exits on: a claim about what
  // a panorama needs. This test's job is different and narrower — to notice when the estimator gets
  // worse — and a bound set generously enough to be a product statement is far too loose for that.
  //
  // Measured in the hangar: the medians run 0.024 to 0.101 and the worst single frame is 0.1551,
  // ORB's. Four places, because this sentence is a claim about a bound: rounding a measurement is
  // fine, and rounding it inside a claim that something does not exceed it is how a sentence here
  // came to be false by a ten thousandth.
  // Deleting the entire inlier refit — the step that makes the answer better than the three points
  // that found it — takes ORB to median 0.2108 and max 0.6477 and fails both bounds, while AKAZE
  // lands at 0.1826/0.2989 and SIFT at 0.0535/0.1147 and both pass. So the bounds are roughly twice
  // the measurement rather than five times it: tight enough that losing a whole stage of the fit is
  // caught, loose enough not to fail on the next OpenCV bump, which is the only thing that should
  // move these numbers at all (the dataset, the seed and the detectors are pinned).
  //
  // **Where else the table is written, because a bump moves all of it and the suite would stay
  // green.** The bounds here are what fail — **both of them**, `EXPECT_LT(score.medianDeg, 0.2)` and
  // `EXPECT_LT(score.maxDeg, 0.4)`, which is worth naming together because a correction that moves
  // one and forgets the other leaves a red suite for a reason the commit message will not mention.
  // The figures are prose in the places listed below, which may be corrected and which nothing
  // invalidates. **There is deliberately no count.** This list has had
  // a total three times — eight, then nine, then ten — and every one of them was left stale by the
  // edit that added an entry, in a comment whose entire job is stopping a figure from going stale
  // somewhere. "Every entry below" is greppable and survives the next `6c`. In this file:
  //
  //   1. this comment's `0.024 to 0.101` and `0.1551` — **and the refit paragraph above it**,
  //      whose `0.2108 / 0.6477`, `0.1826 / 0.2989` and `0.0535 / 0.1147` are hangar measurements
  //      an OpenCV bump moves and no bound asserts. Re-run the refit sabotage to get them;
  //   2. the `Today's run, for comparison:` transcript below — the whole triple;
  //   3. the worst-frame bound's `0.1551`, under `EXPECT_LT(score.maxDeg, 0.4)`;
  //   4. the acceptance docblock's `20 of 141 / 42 of 199 / 61 of 178`;
  //   5. its repeat beside `kFirst`;
  //   5b. the margin `0.2 - 0.1826 = 0.0174` below, which is arithmetic over two of those;
  //   6. the `0.6522` lowest inlier fraction in `World`'s docblock, which is the one figure here
  //      that no bound asserts and no probe in the tree reproduces.
  //
  // **And one thing that is not a figure at all**, listed because ADR 0061 points a reader here for
  // the whole blast radius rather than keeping a second copy of it: the `+ 0.5` that a correction
  // adds to `ReadBearings` in `feature_registration_engine.cpp`. It moves in the same commit as
  // every figure here, and it is the only entry whose absence leaves the tree self-consistent —
  // which is exactly why it is the one that would be forgotten.
  //
  // And outside it:
  //
  //   6b. the `Bearing` docblock in `feature_registration_engine.cpp`, which spells the whole
  //      *today* column in its own table and is not a copy of the roadmap's — it carries a second
  //      column of corrected figures beside it, so a bump moves six numbers there and leaves six
  //      more that are only correct relative to them. It was missing from this list until ADR 0061
  //      went to add a figure to it and a reviewer asked which of the eight that was;
  //   7. `docs/06-roadmap.md`'s table, and separately the prose around it that the table does not
  //      cover — `ORB's median roughly triples` beside the half-pixel paragraph, which carries no
  //      digits on purpose and still has to move when the ratio stops being three;
  //      `the hangar's lowest inlier fraction is 0.6522`, `SIFT's median is 4.2 times
  //      better than ORB's` and `now reads eleven of eleven for all three`. Editing the table
  //      alone leaves all three standing, and a reader who trusts the entry stops at the table;
  //   8. `CLAUDE.md`'s `0.024 … 0.061 … 0.101` — **twice**, once in the paragraph on the pair
  //      estimator and again in the one on `Refine`, as "the chain's 0.101, 0.061 and 0.024";
  //   9. the solved-ring test below, whose docblock quotes the chain's medians beside its own;
  //  10. `docs/06-roadmap.md`'s solved-ring table, whose "Chained median" column is this table's
  //      median column again, and ADR 0065, which quotes it (an ADR, so superseded, not edited).
  //
  // The table rounds to three places and the bounds above claim four, which is how `0.155` and
  // `0.1551` came to name one quantity — keep the rounding where it is and the claims exact.
  //
  // **ADR 0059 carries `0.652` and the live medians, and ADR 0061 carries this file's whole table
  // beside the figures a correction would produce. Neither is an entry here.** An ADR is never
  // edited into agreement with the present (`docs/adr/README.md`), so a bump that moves these
  // figures supersedes both rather than correcting either. That is the same reason the `+ 0.5`
  // above is listed apart from the figure sites: those are things the correction *touches*, and the
  // entries below are figures it *rewrites*.
  // (`docs/adr/0056` is deliberately absent: its numbers are the checkerboard's, which is what that
  // ADR is about, so it is not a copy of this table.)
  //
  // Re-run `--gtest_filter='*Accuracy*'` — it prints `[accuracy] detector=N … median=… max=…` for
  // each — correct every entry in the list above, and supersede ADR 0059 rather than correcting it. Otherwise the next
  // reader inherits a table that was true of a different OpenCV.
  // Today's run, for comparison: ORB 0.1009/0.1091/0.1551, AKAZE 0.0612/0.0673/0.1330, SIFT
  // 0.0239/0.0250/0.0435, eleven of eleven each.
  //
  // **Caught by one instantiation of three, and which one depends on the world** — it was AKAZE in
  // the checkerboard and it is ORB here. That is the argument for keeping all three: any single
  // parameter is a weaker test than a green run makes it look. The thinnest margin that survives the
  // sabotage is AKAZE's median, 0.2 - 0.1826 = 0.0174.
  EXPECT_LT(score.medianDeg, 0.2)
      << "median " << score.medianDeg << " degrees over " << kFrames << " chained frames";

  // **The worst frame, which a median cannot see.** A chain wrong by a degree on every answered
  // step scores a passing median once the gauge is removed and the refused steps re-anchor it to
  // truth; this is what catches that. Four tenths is 2.58 times the worst single frame any detector
  // produces here (0.1551, ORB's) — the same four-place figure the bound above claims, because two
  // spellings of one quantity is what made a sentence here false by a ten thousandth.
  //
  // **What it no longer catches, said out loud because the change that did this is ADR 0059's.**
  // This bound was written when deleting `withinBound` — the half-turn defence — left AKAZE at
  // `max 178.8014` with only the median asserted. In the hangar, deleting `withinBound` at both its
  // sites changes nothing at all: all three detectors return the same numbers to the digit and the
  // suite is green, because the alias it excludes was the checkerboard's half-turn symmetry and a
  // room has none. The defence is still tested — `ThePriorBoundsTheSearchAndTheAnswerStaysInsideIt`
  // fails on all three detectors with the bound gone, and is the only test that does — but it is
  // tested there and not here, which is the better arrangement and is not where a reader of this
  // bound would look.
  //
  // The maintainer was asked whether to loosen these bounds and said to keep the tight margin. What
  // that costs, stated so nobody rediscovers it: when an OpenCV bump moves them, the failure will
  // look like a regression in the estimator and will not be one. Read the printed
  // `median/mean/max` line for all three detectors first — two of three moving by a similar small
  // amount is the library, one moving alone is a real change. A future detector whose honest
  // maximum is above 0.3 does not fit under this bound at all, and the answer then is a per-detector
  // bound rather than one loose enough for the worst of them.
  EXPECT_LT(score.maxDeg, 0.4)
      << "one frame is " << score.maxDeg << " degrees out, which a median cannot see";

}

/**
 * The same ring solved through `Refine`, closing pair included, against priors that are wrong.
 *
 * The test above chains eleven pairs and throws the twelfth away; this one hands all twelve to
 * `Refine` with a prior for every frame, each three degrees out about an axis of its own — the
 * order a working phone is out by, and independent per frame, so the priors cannot agree on a
 * wrong shape. The pairs are estimated exactly as above.
 *
 * **What the closing pair is worth here is the number**, because unlike the solver's own fixture
 * the per-pair errors are independent rather than one drift a closure removes exactly.
 */
TEST_P(Accuracy, TheRingSolvedWithItsClosingPairIsWithinTheStatedBound) {
  constexpr int kFrames = 12;
  Rendered rendered(kFrames, 640, 480, World::Photograph);
  ASSERT_FALSE(rendered.inputMissing()) << rendered.why();
  if (!rendered.ok()) {
    GTEST_SKIP() << "the dataset generator did not run, so nothing was measured. " << rendered.why();
  }

  MemoryFrameStoreAccess store{1 << 28};
  Owned owned{store};
  const Result<SyntheticDataset> dataset = LoadSyntheticDataset(store, rendered.path());
  ASSERT_TRUE(dataset.ok()) << dataset.status.detail;
  for (const SyntheticFrame& frame : dataset.value.frames) owned.frames.push_back(frame.frame);
  ASSERT_EQ(dataset.value.frames.size(), static_cast<size_t>(kFrames));

  FeatureRegistrationEngine engine{store, GetParam()};
  std::vector<FeatureSet>& sets = owned.sets;
  std::vector<Quat> truth;
  for (const SyntheticFrame& frame : dataset.value.frames) {
    const Result<FeatureSet> features = engine.ExtractFeatures(frame.frame);
    ASSERT_TRUE(features.ok()) << features.status.detail;
    sets.push_back(features.value);
    truth.push_back(frame.trueRotation);
  }

  std::vector<PairwiseResult> pairs;
  for (int at = 0; at < kFrames; ++at) {
    const size_t a = static_cast<size_t>(at);
    const size_t b = static_cast<size_t>((at + 1) % kFrames);
    const Quat nudge = FromAxisAngle(Vec3{1, 0, 0}, 3.0 * std::numbers::pi / 180.0);
    const Quat prior = Normalize(Multiply(Multiply(Conjugate(truth[b]), truth[a]), nudge));
    const Result<PairwiseResult> pair =
        engine.EstimatePairwise(sets[a], sets[b], prior, dataset.value.lens);
    ASSERT_TRUE(pair.ok()) << "pair " << a << "-" << b << ": " << pair.status.detail;
    pairs.push_back(pair.value);
  }

  std::vector<FramePrior> priors;
  for (size_t i = 0; i < truth.size(); ++i) {
    const double at = static_cast<double>(i);
    const Vec3 axis{std::sin(at), std::cos(at), std::sin(2.0 * at)};
    FramePrior prior;
    prior.frame = dataset.value.frames[i].frame.id;
    prior.pose.orientation =
        Normalize(Multiply(truth[i], FromAxisAngle(axis, 3.0 * std::numbers::pi / 180.0)));
    prior.pose.confidence = 1.0;   // anchored, as every burst-captured frame's is (ADR 0044)
    priors.push_back(prior);
  }

  const Result<GlobalSolution> solved = engine.Refine(pairs, priors, dataset.value.lens);
  ASSERT_TRUE(solved.ok()) << solved.status.detail;
  ASSERT_EQ(solved.value.rotations.size(), truth.size());
  EXPECT_TRUE(solved.value.converged);
  // Fitted even handed the rendered lens: the figures below are the refitted pairs' solve, and a
  // pass-through would print the unrefitted ones and still sit inside these bounds (round 4).
  EXPECT_TRUE(solved.value.lensFitted);
  EXPECT_EQ(solved.value.edgesUsed, kFrames) << "not every pair was accepted";

  const test::RotationScore score = test::ScoreRotations(solved.value.rotations, truth);
  ASSERT_TRUE(score.valid && score.alignmentIsUnique);
  // `[solved]` rather than `[accuracy]`: the gate counts `[accuracy]` lines against the chained
  // test's instantiations to prove the measurement ran.
  std::fprintf(stderr, "[solved] detector=%d edges=%d median=%.4f deg mean=%.4f max=%.4f\n",
               static_cast<int>(GetParam()), solved.value.edgesUsed, score.medianDeg,
               score.meanDeg, score.maxDeg);

  // **Facing where the priors agree**, since that is the one thing they are there to decide. The
  // priors' own agreed gauge is 0.26 degrees from the truth; the solve lands within 0.0002 of it.
  std::vector<Quat> orientations;
  for (const FramePrior& prior : priors) orientations.push_back(prior.pose.orientation);
  const test::GaugeAlignment agreed = test::BestGaugeAlignment(orientations, truth);
  ASSERT_TRUE(agreed.valid);
  EXPECT_LT(AngleBetween(agreed.rotation, score.alignment) * 180.0 / std::numbers::pi, 0.001);

  // Today's run: ORB 0.0464/0.0510/0.1408, AKAZE 0.0293/0.0332/0.0660, SIFT 0.0142/0.0144/0.0331
  // (median, mean, max), against the chain's 0.1009, 0.0612 and 0.0239 medians, with the focal
  // length fitted to -0.058%, +0.002% and +0.012% of the rendered one. Before the fit (ADR 0065)
  // these read 0.0552, 0.0348 and 0.0264, and SIFT's was a little worse than its chain for no known
  // reason. The reason was the pairs, not the solve: `FitRotation` returns the rotation fitted on its
  // inlier set *before* the re-gate and reports the set after it, and `Refine` now refits every pair
  // on its reported inliers — which alone, at the rendered focal length with no search, gives
  // 0.0375, 0.0291 and 0.0147 (ADR 0066). Written also in `docs/06-roadmap.md`, `CLAUDE.md` and
  // `docs/05-toolchain-and-testing.md`, which move with these; the chain's list above does not
  // cover them.
  //
  // The median's bound is what fails a solve that has lost the closing pair: handed eleven, ORB
  // reads 0.1005, AKAZE 0.0601 and SIFT 0.0229, and only ORB's is past 0.08 — which is why the
  // bound is tighter than twice the measurement, and why `edgesUsed` is asserted above as well.
  EXPECT_LT(score.medianDeg, 0.08);
  EXPECT_LT(score.maxDeg, 0.25);
}

/**
 * A focal length out by the margin a phone's reported field of view can be is fitted from the ring,
 * and the ring then solves as though it had been right (ADR 0066).
 *
 * The same guess goes to `EstimatePairwise` and to `Refine`, as it would on a phone: nothing else
 * knows the lens. Measured first, bounded after, as the rest of this file is.
 */
TEST_P(Accuracy, AFocalLengthOutIsFittedFromTheRing) {
  constexpr int kFrames = 12;
  Rendered rendered(kFrames, 640, 480, World::Photograph);
  ASSERT_FALSE(rendered.inputMissing()) << rendered.why();
  if (!rendered.ok()) {
    GTEST_SKIP() << "the dataset generator did not run, so nothing was measured. " << rendered.why();
  }

  MemoryFrameStoreAccess store{1 << 28};
  Owned owned{store};
  const Result<SyntheticDataset> dataset = LoadSyntheticDataset(store, rendered.path());
  ASSERT_TRUE(dataset.ok()) << dataset.status.detail;
  for (const SyntheticFrame& frame : dataset.value.frames) owned.frames.push_back(frame.frame);

  FeatureRegistrationEngine engine{store, GetParam()};
  std::vector<FeatureSet>& sets = owned.sets;
  std::vector<Quat> truth;
  for (const SyntheticFrame& frame : dataset.value.frames) {
    const Result<FeatureSet> features = engine.ExtractFeatures(frame.frame);
    ASSERT_TRUE(features.ok()) << features.status.detail;
    sets.push_back(features.value);
    truth.push_back(frame.trueRotation);
  }
  std::vector<FramePrior> priors;
  for (size_t i = 0; i < truth.size(); ++i) {
    const double at = static_cast<double>(i);
    const Vec3 axis{std::sin(at), std::cos(at), std::sin(2.0 * at)};
    FramePrior prior;
    prior.frame = dataset.value.frames[i].frame.id;
    prior.pose.orientation =
        Normalize(Multiply(truth[i], FromAxisAngle(axis, 3.0 * std::numbers::pi / 180.0)));
    prior.pose.confidence = 1.0;
    priors.push_back(prior);
  }

  for (const double scale : {0.90, 0.95, 1.05, 1.10}) {
    Intrinsics guess = dataset.value.lens;
    guess.fx *= scale;
    guess.fy *= scale;
    std::vector<PairwiseResult> pairs;
    for (int at = 0; at < kFrames; ++at) {
      const size_t a = static_cast<size_t>(at);
      const size_t b = static_cast<size_t>((at + 1) % kFrames);
      const Quat nudge = FromAxisAngle(Vec3{1, 0, 0}, 3.0 * std::numbers::pi / 180.0);
      const Quat prior = Normalize(Multiply(Multiply(Conjugate(truth[b]), truth[a]), nudge));
      const Result<PairwiseResult> pair = engine.EstimatePairwise(sets[a], sets[b], prior, guess);
      ASSERT_TRUE(pair.ok()) << scale << " pair " << a << "-" << b << ": " << pair.status.detail;
      pairs.push_back(pair.value);
    }
    const Result<GlobalSolution> solved = engine.Refine(pairs, priors, guess);
    ASSERT_TRUE(solved.ok()) << scale << ": " << solved.status.detail;
    const test::RotationScore score = test::ScoreRotations(solved.value.rotations, truth);
    ASSERT_TRUE(score.valid);
    const double fxOut = solved.value.intrinsics.fx / dataset.value.lens.fx - 1.0;
    std::fprintf(stderr,
                 "[focal] detector=%d scale=%.2f fitted=%d fx=%+.4f%% median=%.4f max=%.4f "
                 "edge=%.4f spread=%.4f%% model=%.4f%%\n",
                 static_cast<int>(GetParam()), scale, solved.value.lensFitted ? 1 : 0,
                 100.0 * fxOut, score.medianDeg, score.maxDeg, solved.value.medianEdgeErrorDeg,
                 100.0 * solved.value.focalSpread, 100.0 * solved.value.focalModelError);
    EXPECT_TRUE(solved.value.lensFitted) << scale;
    // Today's run, across the four scales: the focal length within -0.063 to -0.051% (ORB), -0.004
    // to +0.002% (AKAZE) and +0.010 to +0.012% (SIFT); medians 0.047 to 0.052, 0.029 to 0.034 and
    // 0.010 to 0.019; worst frames at most 0.146, 0.070 and 0.038; edge medians at most 0.018,
    // 0.007 and 0.005 (ADR 0066). Unfitted, the median at 2% out was already 0.50 (ORB).
    //
    // **Per detector**, because one bound for all three was ORB's, and a fit biased 0.15% high or a
    // search stopped at a hundredth passed for the two that fit far better. About one and a half to
    // two times each measurement, in the manner of the rest of this file.
    struct Bound {
      double focal, median, max, edge;
    };
    constexpr Bound kBounds[] = {{0.0010, 0.08, 0.25, 0.03},     // ORB
                                 {0.0001, 0.05, 0.12, 0.012},    // AKAZE
                                 {0.0002, 0.03, 0.06, 0.008}};   // SIFT
    static_assert(std::size(kBounds) == static_cast<size_t>(FeatureDetector::Count));
    const Bound& bound = kBounds[static_cast<size_t>(GetParam())];
    EXPECT_LT(std::abs(fxOut), bound.focal) << scale;
    EXPECT_LT(score.medianDeg, bound.median) << scale;
    EXPECT_LT(score.maxDeg, bound.max) << scale;
    EXPECT_LT(solved.value.medianEdgeErrorDeg, bound.edge) << scale;

    // **And with the pairs noisier than the render**, 1.5 px added to every match, which is a sharp
    // frame by a phone's standard. A rule that judged a fit by how little the loops failed to close
    // at it refused four seeds of ten for ORB here — six in a reviewer's own draws — sending back
    // the lens 5% long and a median past a degree, with the least itself within 0.25% (round 4).
    // Measured on these seeds: every one fitted, the focal length within 0.21% (ORB), 0.06% (AKAZE)
    // and 0.05% (SIFT), on a precision of 0.12 to 0.14%, 0.05% and 0.02%.
    if (scale != 1.05) continue;
    constexpr double kNoisyFocal[] = {0.004, 0.001, 0.001};
    static_assert(std::size(kNoisyFocal) == static_cast<size_t>(FeatureDetector::Count));
    for (uint32_t seed = 1; seed <= 10; ++seed) {
      const Result<GlobalSolution> noisy =
          engine.Refine(test::WithNoise(pairs, 1.5, seed), priors, guess);
      ASSERT_TRUE(noisy.ok()) << "seed " << seed << ": " << noisy.status.detail;
      EXPECT_TRUE(noisy.value.lensFitted) << "seed " << seed;
      EXPECT_LT(std::abs(noisy.value.intrinsics.fx / dataset.value.lens.fx - 1.0),
                kNoisyFocal[static_cast<size_t>(GetParam())])
          << "seed " << seed;
    }
  }
}

/**
 * A refusal and an unaccepted answer are different outcomes, and both happen here.
 *
 * **ADR 0056's central claim, asserted.** `accepted` earns its place in `PairwiseResult` only if it
 * can be false on a result that was returned — otherwise it says exactly what `Result::ok()` says,
 * which is the defect round 1 found and which nothing could see: the fix was in the code, no test
 * asserted `accepted == false` anywhere, and a reviewer restored the constant-`true` behaviour with
 * all 798 tests green.
 *
 * **Rendered from the checkerboard**, which is now a choice rather than the only world there is: see
 * `World` above for the sweep that says the photograph cannot produce this outcome at all. The
 * numbers quoted below are the checkerboard's and are unchanged by ADR 0059.
 *
 * Written as a statement about the *engine and the dataset together* rather than about a detector,
 * because "ORB declines frames 0 and 1" is a fact that a better matcher should be free to change.
 * What must not change is that the two outcomes come apart: some pair of a clean ring produces a
 * rotation a minority of its correspondences agree with, and some pair produces one a healthy share
 * do. Restating the acceptance rule as an assertion would only check the code against itself.
 *
 * Measured here, so the arrangement is known to reach both branches: on frames 3 and 4 against a
 * prior three degrees out, ORB answers with 20 inliers of 141 correspondences and is not accepted,
 * while AKAZE answers with 42 of 199 and SIFT with 61 of 178, both accepted. ORB's truth rotation on
 * that pair is itself agreed on by only 19 of 141, so the estimator is finding as much as the scene
 * allows and reporting honestly that it is not much.
 */
TEST(Acceptance, AnAnswerWithAMinorityBehindItIsReturnedAndNotAccepted) {
  Rendered rendered(12, 640, 480, World::Checkerboard);
  // Asserted here too, though a checkerboard has no input to be missing: the guard is conditional on
  // the world, so keeping the two in step is otherwise done by hand at every call site.
  ASSERT_FALSE(rendered.inputMissing()) << rendered.why();
  if (!rendered.ok()) {
    GTEST_SKIP() << "the dataset generator did not run, so nothing was measured. "
                 << rendered.why();
  }

  MemoryFrameStoreAccess store{1 << 28};
  // Declared before anything is allocated, so it outlives every `ASSERT_*` below and unwinds last.
  Owned owned{store};
  const Result<SyntheticDataset> dataset = LoadSyntheticDataset(store, rendered.path());
  ASSERT_TRUE(dataset.ok()) << dataset.status.detail;
  for (const SyntheticFrame& frame : dataset.value.frames) owned.frames.push_back(frame.frame);

  // **Frames 3 and 4, against a prior three degrees from truth — and both halves of that matter.**
  //
  // The first version of this test used frames 0 and 1 and handed over the *exact* truth of the
  // step, which is the arrangement `0561561` removed from the accuracy test for making the
  // measurement a mirror. A reviewer showed the same flaw here: an estimator gutted to return its
  // prior passes, because the prior is the answer and its inlier count is truth's, so ORB declines
  // and SIFT accepts exactly as they should. The existence property held for an engine that read no
  // pixels at all.
  //
  // It cannot be repaired by nudging *that* pair. Sweeping the perturbation: at 0.25 degrees the
  // echo still survives (a quarter degree is about 2 px at this focal length, inside the 3 px inlier
  // radius) and at 0.5 degrees ORB stops answering altogether, so the declined case vanishes. The
  // window where the echo dies and the window where ORB answers do not overlap on frames 0 and 1.
  //
  // They do on frames 3 and 4, which is where the accuracy measurement already shows ORB answering
  // and declining under the full three degrees: 20 **inliers** of 141 correspondences, against
  // AKAZE's 42 of 199 and SIFT's 61 of 178, both accepted — the same triple `Acceptance`'s docblock
  // carries, and the word `inliers` is load-bearing in both, because "20 correspondences of 141"
  // names the numerator after the denominator and is the spelling one of the two copies had.
  // Three degrees is about 26 px, so an echo gathers
  // nothing and is refused — which fails the "something was accepted" half and catches it.
  const size_t kFirst = 3;
  // `Rendered` above asks for twelve frames and this asks for two of them by index; nothing else
  // keeps that pair in step. Without this, shrinking the ring reads a `FrameRef` past the end of a
  // vector reserved to exactly its count, hands it to `ExtractFeatures`, and fails blaming
  // extraction. `Accuracy` asserts its own count with `ASSERT_EQ(dataset.value.frames.size(),
  // static_cast<size_t>(kFrames))`; this did not.
  ASSERT_GT(dataset.value.frames.size(), kFirst + 1)
      << "the dataset has " << dataset.value.frames.size() << " frames and this test reads index "
      << (kFirst + 1);
  const Quat truthStep = Multiply(Conjugate(dataset.value.frames[kFirst + 1].trueRotation),
                                  dataset.value.frames[kFirst].trueRotation);
  const Quat nudge = FromAxisAngle(Vec3{1, 0, 0}, 3.0 * std::numbers::pi / 180.0);
  const Quat step = Normalize(Multiply(truthStep, nudge));

  int accepted = 0;
  int answeredButNotAccepted = 0;
  for (const FeatureDetector detector : kAllFeatureDetectors) {
    FeatureRegistrationEngine engine{store, detector};
    // Its own holder, because the assertion below can fire with `a` extracted and `b` refused — and
    // because the dataset's frames are still held here, so this one gives back without asserting
    // the store is empty. Only the outer holder can say that, and only after this one has run.
    Owned extracted{store, Owned::Then::kJustGiveBack};
    const Result<FeatureSet> a = engine.ExtractFeatures(dataset.value.frames[kFirst].frame);
    if (a.ok()) extracted.sets.push_back(a.value);
    const Result<FeatureSet> b = engine.ExtractFeatures(dataset.value.frames[kFirst + 1].frame);
    if (b.ok()) extracted.sets.push_back(b.value);
    ASSERT_TRUE(a.ok() && b.ok());
    const Result<PairwiseResult> pair =
        engine.EstimatePairwise(a.value, b.value, step, dataset.value.lens);
    if (pair.ok()) {
      // **The denominator is carried, so this is checkable at all.** `accepted` is a comparison
      // against a fraction, and until `correspondences` existed a caller held the numerator alone —
      // eleven agreeing out of forty and out of a hundred and twenty-eight are the same `inliers`
      // and are not the same evidence.
      EXPECT_GT(pair.value.correspondences, 0)
          << "an answer was returned without saying how many correspondences it is out of";
      EXPECT_LE(pair.value.inliers, pair.value.correspondences);
      if (pair.value.accepted) {
        ++accepted;
      } else {
        ++answeredButNotAccepted;
        // Not a refusal: the rotation is there to be read. `Refine` leaves such a pair out
        // (ADR 0065), but it is still an answer where a refusal is none — the whole distinction
        // the field exists to carry. `inliers > 0` is not
        // asserted — an answer cannot come back with fewer than `kMinimumCorrespondences`, so it
        // could not fail, and a reviewer was right that it reads as a check while being a
        // restatement of the refusal floor.
        EXPECT_LT(pair.value.inliers * 5, pair.value.correspondences)
            << "not accepted, yet a fifth or more of the correspondences agree, which is not what "
               "the gate says";
      }
    }
  }

  EXPECT_GT(answeredButNotAccepted, 0)
      << "every detector that answered was accepted, so `accepted` says nothing `ok()` does not — "
         "which is the defect ADR 0056 exists to have fixed";
  EXPECT_GT(accepted, 0) << "nothing was accepted at all, so this proves only that the gate refuses";
}

INSTANTIATE_TEST_SUITE_P(EveryDetector, Accuracy,
                         // From the engine's own list, as the extraction suite already does, rather
                         // than the three restated here — a detector added to `FeatureDetector`
                         // would otherwise reach `Make()` and never be measured. That matters more
                         // for this suite than for any other: CI now gates on the name
                         // `EveryDetector/Accuracy.*`, so "every detector" has to be true of it.
                         ::testing::ValuesIn(kAllFeatureDetectors));

}  // namespace
}  // namespace sphanorama
