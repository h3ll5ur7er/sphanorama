/**
 * **How wrong are the rotations?** This is the measurement Phase 2 exits on, and until this file
 * existed nothing in the repository answered it.
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
    const Quat nudge = FromAxisAngle(Vec3{1, 0, 0}, 3.0 * 3.14159265358979323846 / 180.0);
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
  // **And the alignment it found is the only one.** `maxDeg` and `medianDeg` are measured after the
  // gauge is removed, so they are statements about the *residual* once a common rotation has been
  // divided out — and if that rotation is not unique, they are statements about an arbitrary choice
  // among several. Every assertion below reads those two numbers; this is what makes them mean
  // something. A reviewer pointed out that the branch read them while checking only `valid`.
  ASSERT_TRUE(score.alignmentIsUnique)
      << "the gauge alignment is degenerate, so the residual below is one of several answers";

  const int steps = kFrames - 1;
  std::fprintf(
      stderr,
      "[accuracy] detector=%d frames=%d registered=%d/%d median=%.4f deg mean=%.4f max=%.4f\n",
      static_cast<int>(GetParam()), kFrames, steps - unregistered, steps, score.medianDeg,
      score.meanDeg, score.maxDeg);

  // **The measurement has to beat its own prior, or it is measuring the prior.** Each step is handed
  // a rotation three degrees from truth; chaining eleven of those unimproved would drift far past
  // this. Asserting it is what stops the estimator quietly degenerating into an echo again — the
  // failure this whole test had when it was written, which no assertion in it could see.
  EXPECT_LT(score.medianDeg, 3.0)
      << "the chain is no better than the three-degree prior it was seeded with, so this is "
         "measuring the sensor rather than the registration";

  // **A majority of the ring, not all of it — and the difference is a measurement rather than a
  // concession.** The first version of this line demanded every step, and ORB failed three of
  // eleven. Instrumenting the engine to count inliers under the *truth* rotation settled what those
  // three were: on those pairs the correct rotation itself is agreed on by 11 of 128, 19 of 141 and
  // 13 of 154 correspondences, and RANSAC returned 20 and 13 on the two it answered — as well as is
  // possible. The first is a refusal rather than a declined answer, which is a distinction worth
  // keeping straight here of all places. Nine in ten of ORB's surviving matches on those pairs are wrong, because a
  // checkerboard panorama gives it hundreds of corners that are genuinely indistinguishable and
  // Lowe's ratio cannot separate what is not separable. `accepted` was false because the support
  // really was a minority, which is the field doing its job.
  //
  // So the honest bar is the one a capture actually needs: a detector that cannot register more
  // than half the consecutive pairs of a clean ring cannot drive a sphere, whatever its accuracy on
  // the ones it does. That is a statement about usability and not an echo of what was measured —
  // which is why it is a half and not the eight-elevenths ORB scores.
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
  EXPECT_LT(score.medianDeg, 0.5)
      << "median " << score.medianDeg << " degrees over " << kFrames << " chained frames";

  // **The worst frame, which is the number the prior's bound exists to hold down.** A reviewer
  // deleted `withinBound` — the entire half-turn defence — and this test passed on two of three
  // detectors: AKAZE came back `median 0.4034` with `mean 44.8946` and `max 178.8014`, and only the
  // median was asserted. A median is a statement about the typical frame and the alias is not
  // typical; it is one or two frames turned most of the way round, which is exactly the shape
  // `rotation_scoring.h` says the maximum is there to make legible. The number reached stderr and no
  // assertion.
  //
  // One degree: about four times the worst single frame any detector actually produces here (0.26)
  // and two orders of magnitude under the alias. It is also what catches the other way this test can
  // be flattered — a chain wrong by a degree on every answered step scores a passing median once the
  // gauge is removed and the refused steps re-anchor it to truth, and cannot hide from this.
  EXPECT_LT(score.maxDeg, 1.0)
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
 * Measured here, so the arrangement is known to reach both branches: on frames 0 and 1, ORB answers
 * with 11 inliers of 128 correspondences and is not accepted, while SIFT answers with 60 of 181 and
 * is. ORB's truth rotation on that pair is itself agreed on by only 11 of 128 correspondences, so
 * the estimator is doing about as well as the scene allows and reporting honestly that it is not
 * much.
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

  const Quat step = Normalize(Multiply(Conjugate(dataset.value.frames[1].trueRotation),
                                       dataset.value.frames[0].trueRotation));

  int accepted = 0;
  int answeredButNotAccepted = 0;
  for (const FeatureDetector detector : kAllFeatureDetectors) {
    FeatureRegistrationEngine engine{store, detector};
    const Result<FeatureSet> a = engine.ExtractFeatures(dataset.value.frames[0].frame);
    const Result<FeatureSet> b = engine.ExtractFeatures(dataset.value.frames[1].frame);
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
