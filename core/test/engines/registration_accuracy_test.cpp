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
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <vector>

#include "engines/registration_engine/feature_registration_engine.h"
#include "resource_access/frame_store_access/memory_frame_store_access.h"
#include "support/rotation_scoring.h"
#include "support/synthetic_dataset.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

namespace fs = std::filesystem;

/** Where the generator lives, relative to the committed data directory this build already knows. */
std::string RepoRoot() {
  return fs::path(SPHANORAMA_TEST_DATA_DIR).parent_path().parent_path().parent_path().string();
}

/**
 * One argument, safe to hand to `/bin/sh`.
 *
 * **Not paranoia about a hostile checkout — a checkout in `/home/o'brien`.** Two of the strings
 * below are paths this test does not choose: the repository root arrives as a compile-time define
 * and the scratch directory from `TMPDIR`. Wrapping them in single quotes and hoping was the first
 * version, and a single apostrophe anywhere in either ends the quote and hands the rest of the path
 * to the shell as words — which on a path with a space in it is a command. The POSIX escape for an
 * apostrophe inside single quotes is to close, emit an escaped one, and reopen.
 */
std::string Quoted(const std::string& raw) {
  std::string out = "'";
  for (const char letter : raw) {
    if (letter == '\'') {
      out += "'\\''";
    } else {
      out += letter;
    }
  }
  return out + "'";
}

/**
 * A rendered ring, or nothing.
 *
 * Shelling out rather than linking: the generator is Python and stays Python for the reason ADR
 * 0050 gives — a dataset rendered through the code under test cancels any error the two share.
 *
 * `--locked` rather than a bare `uv run`: without it a stale lock file is *resolved and rewritten*,
 * so a test run would leave a modified `uv.lock` in the working tree and the measurement would have
 * been taken against dependencies nobody chose. With it, `uv` refuses and this skips instead — a
 * skipped measurement being the honest outcome when the environment is not the one that was pinned
 * (ADR 0048).
 */
class Rendered {
 public:
  Rendered(int frames, int edgeWidth, int edgeHeight) {
    // **`mkdtemp`, not a name built from the pid.** `TMPDIR` is usually world-writable and pids
    // recycle, so the previous `sphanorama-accuracy-<pid>` could already exist and belong to someone
    // else — and it was cleared with the *throwing* `remove_all` overload, so a directory this
    // process cannot delete aborted the constructor instead of skipping. `mkdtemp` creates the
    // directory itself, at 0700, with a name nobody can predict, and fails rather than reusing.
    std::string pattern = (fs::temp_directory_path() / "sphanorama-accuracy-XXXXXX").string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (::mkdtemp(buffer.data()) == nullptr) {
      why_ = "could not create a temporary directory under " +
             fs::temp_directory_path().string();
      return;
    }
    path_ = fs::path(buffer.data());
    made_ = true;
    // The generator refuses an `--out` that already exists as a non-directory and clears one that
    // does, so handing it the empty directory `mkdtemp` just made is exactly what it expects.

    // **Its output is kept, not sent to `/dev/null`.** Every way this can fail used to arrive as
    // the same skip message — "`uv` and the `datasets` group are needed" — including a renderer
    // crash, a lock file that no longer resolves, and a checkout path that broke the shell. A skip
    // that misdiagnoses its own cause is worse than one that says nothing.
    const fs::path log = path_.parent_path() / (path_.filename().string() + ".log");
    const std::string command =
        "cd " + Quoted(RepoRoot()) +
        " && uv run --locked --group datasets tools/synth_dataset.py --out " +
        Quoted(path_.string()) + " --frames " + std::to_string(frames) + " --width " +
        std::to_string(edgeWidth) + " --height " + std::to_string(edgeHeight) + " >" +
        Quoted(log.string()) + " 2>&1";
    const int status = std::system(command.c_str());
    ok_ = status == 0 && fs::exists(path_ / "truth.json");
    if (!ok_) why_ = "the renderer exited " + std::to_string(status) + ": " + Tail(log);
    std::error_code ignored;
    fs::remove(log, ignored);
  }
  ~Rendered() {
    if (!made_) return;
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  Rendered(const Rendered&) = delete;
  Rendered& operator=(const Rendered&) = delete;

  bool ok() const { return ok_; }
  std::string path() const { return path_.string(); }
  /** Why it did not render, for the skip message. Empty when it did. */
  const std::string& why() const { return why_; }

 private:
  /** The last few lines of the renderer's output, which is where its complaint is. */
  static std::string Tail(const fs::path& log) {
    std::ifstream stream(log);
    if (!stream) return "(no output was captured)";
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(stream, line)) lines.push_back(line);
    if (lines.empty()) return "(it said nothing)";
    std::string tail;
    for (size_t at = lines.size() > 5 ? lines.size() - 5 : 0; at < lines.size(); ++at) {
      tail += lines[at];
      tail += "\n";
    }
    return tail;
  }

  fs::path path_;
  std::string why_;
  bool ok_ = false;
  bool made_ = false;
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

/**
 * The quoting survives a real shell, not a shell we imagine.
 *
 * **Asserted by running one**, because the failure being guarded against is the shell's parse and
 * not the string's contents — a test comparing `Quoted(x)` against an expected literal would pass
 * for an escape that `sh` reads differently from the author. `printf %s` writes the argument back
 * with nothing added, so what comes out is exactly what the shell decided the argument was.
 */
TEST(ShellQuoting, APathWithAnApostropheReachesTheCommandIntact) {
  const std::string awkward = "/home/o'brien/my repo/$(touch pwned)/`id`; rm -rf x";
  const std::string command = "printf %s " + Quoted(awkward);
  std::FILE* pipe = ::popen(command.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::string seen;
  char chunk[256];
  while (std::fgets(chunk, sizeof(chunk), pipe) != nullptr) seen += chunk;
  ASSERT_EQ(::pclose(pipe), 0);
  EXPECT_EQ(seen, awkward);
}

class Accuracy : public ::testing::TestWithParam<FeatureDetector> {};

TEST_P(Accuracy, ConsecutiveFramesOfARingRegisterToWithinTheStatedBound) {
  constexpr int kFrames = 12;
  Rendered rendered(kFrames, 640, 480);
  if (!rendered.ok()) {
    GTEST_SKIP() << "the dataset generator did not run, so nothing was measured — and a skipped "
                    "measurement is not a passing one. "
                 << rendered.why();
  }

  MemoryFrameStoreAccess store{1 << 28};
  const Result<SyntheticDataset> dataset = LoadSyntheticDataset(store, rendered.path());
  ASSERT_TRUE(dataset.ok()) << dataset.status.detail;
  ASSERT_EQ(dataset.value.frames.size(), static_cast<size_t>(kFrames));

  FeatureRegistrationEngine engine{store, GetParam()};

  std::vector<FeatureSet> sets;
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

  EXPECT_GT(steps - unregistered, steps / 2)
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
  // Measured: the medians are 0.063 to 0.097 and no single frame exceeds 0.219. Deleting the entire
  // inlier refit — the step that makes the answer better than the three points that found it —
  // moves AKAZE to median 0.4744 and max 0.9607, which cleared both of the previous bounds by a few
  // percent and left the whole suite green (802 tests, as it stood then). So the bounds are set at
  // roughly twice the measurement rather than five times it: tight enough that losing a whole stage
  // of the fit is caught, loose enough not to fail on the next OpenCV bump, which is the only thing
  // that should move these numbers at all (the dataset, the seed and the detectors are pinned).
  //
  // How narrowly it is caught — by one of the three detectors, not by all of them — is measured
  // in full at the `maxDeg` assertion below. Read that before trusting a green run here.
  EXPECT_LT(score.medianDeg, 0.2)
      << "median " << score.medianDeg << " degrees over " << kFrames << " chained frames";

  // **The worst frame, which is the number the prior's bound exists to hold down.** A reviewer
  // deleted `withinBound` — the entire half-turn defence — and this test passed on two of three
  // detectors: AKAZE came back `median 0.4034` with `mean 44.8946` and `max 178.8014`, and only the
  // median was asserted. A median is a statement about the typical frame and the alias is not
  // typical; it is one or two frames turned most of the way round, which is exactly the shape
  // `rotation_scoring.h` says the maximum is there to make legible. The number reached stderr and no
  // assertion.
  //
  // Four tenths of a degree: about twice the worst single frame any detector actually produces here
  // (0.219), and two orders of magnitude under the alias. It is also what catches the other way this test can
  // be flattered — a chain wrong by a degree on every answered step scores a passing median once the
  // gauge is removed and the refused steps re-anchor it to truth, and cannot hide from this.
  //
  // **This margin is thin, and the thinness is a choice rather than an oversight — here is the
  // whole measurement.** Deleting the inlier refit and running all three instantiations gives:
  //
  //     ORB    registered 7/11   median 0.0837   max 0.3394   passes
  //     AKAZE  registered 11/11  median 0.4744   max 0.9607   fails both bounds
  //     SIFT   registered 11/11  median 0.1489   max 0.3297   passes
  //
  // So losing a whole stage of the fit is caught by **AKAZE's instantiation alone**. ORB and SIFT
  // pass every assertion in this test with the refit gone: SIFT comes within 0.0511 of the median
  // bound and 0.0703 of this one, and ORB barely moves at all because on this dataset its answers are
  // already dominated by how few correspondences survive. A parameterised suite whose sabotage is
  // caught by one of three parameters is thinner than it looks from any single passing run, and
  // saying so here is cheaper than the next person re-deriving it.
  //
  // The maintainer was asked whether to loosen these bounds and said to keep the tight margin. That
  // is the right call while the dataset, the seed and the detector parameters are all pinned —
  // nothing but an OpenCV bump can move these numbers. What it costs, stated so nobody has to
  // rediscover it: when a bump does move them, the failure will look like a regression in the
  // estimator and will not be one. Read the printed `median/mean/max` line for all three detectors
  // first. Two of three moving by a similar small amount is the library, and the bounds want
  // re-measuring rather than the code. One moving alone is a real change. And a future detector
  // whose honest maximum is above 0.3 does not fit under this bound at all — the answer then is a
  // per-detector bound, not one loose enough for the worst of them.
  EXPECT_LT(score.maxDeg, 0.4)
      << "one frame is " << score.maxDeg << " degrees out, which a median cannot see";

  for (const FeatureSet& set : sets) {
    (void)store.Forget(set.descriptors);
    (void)store.Forget(set.keypoints);
  }
  for (const SyntheticFrame& frame : dataset.value.frames) (void)store.Forget(frame.frame);
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
  Rendered rendered(12, 640, 480);
  if (!rendered.ok()) {
    GTEST_SKIP() << "the dataset generator did not run, so nothing was measured. "
                 << rendered.why();
  }

  MemoryFrameStoreAccess store{1 << 28};
  const Result<SyntheticDataset> dataset = LoadSyntheticDataset(store, rendered.path());
  ASSERT_TRUE(dataset.ok()) << dataset.status.detail;

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
  // and declining under the full three degrees: 20 correspondences of 141, against AKAZE's 42 of
  // 199 and SIFT's 61 of 178, both accepted. Three degrees is about 26 px, so an echo gathers
  // nothing and is refused — which fails the "something was accepted" half and catches it.
  const size_t kFirst = 3;
  const Quat truthStep = Multiply(Conjugate(dataset.value.frames[kFirst + 1].trueRotation),
                                  dataset.value.frames[kFirst].trueRotation);
  const Quat nudge = FromAxisAngle(Vec3{1, 0, 0}, 3.0 * std::numbers::pi / 180.0);
  const Quat step = Normalize(Multiply(truthStep, nudge));

  int accepted = 0;
  int answeredButNotAccepted = 0;
  for (const FeatureDetector detector : kAllFeatureDetectors) {
    FeatureRegistrationEngine engine{store, detector};
    const Result<FeatureSet> a = engine.ExtractFeatures(dataset.value.frames[kFirst].frame);
    const Result<FeatureSet> b = engine.ExtractFeatures(dataset.value.frames[kFirst + 1].frame);
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
        // Not a refusal: the rotation is there to be read, and a global solve may use it as a weak
        // constraint. That is the whole distinction the field exists to carry. `inliers > 0` is not
        // asserted — an answer cannot come back with fewer than `kMinimumCorrespondences`, so it
        // could not fail, and a reviewer was right that it reads as a check while being a
        // restatement of the refusal floor.
        EXPECT_LT(pair.value.inliers * 5, pair.value.correspondences)
            << "not accepted, yet a fifth or more of the correspondences agree, which is not what "
               "the gate says";
      }
    }
    (void)store.Forget(a.value.descriptors);
    (void)store.Forget(a.value.keypoints);
    (void)store.Forget(b.value.descriptors);
    (void)store.Forget(b.value.keypoints);
  }
  for (const SyntheticFrame& frame : dataset.value.frames) (void)store.Forget(frame.frame);

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
