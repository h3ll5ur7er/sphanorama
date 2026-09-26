#pragma once
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace sphanorama::test {

namespace fs = std::filesystem;

/** Where the generator lives, relative to the committed data directory this build already knows. */
inline std::string RepoRoot() {
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
inline std::string Quoted(const std::string& raw) {
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
 * The two worlds a dataset can be rendered in, and why there are two.
 *
 * `Photograph` is where accuracy is measured. A checkerboard is periodic and infinitely sharp, so a
 * match onto the wrong square agrees with as many neighbours as a match onto the right one, and a
 * median error computed there describes a scene no lens will ever see (ADR 0059).
 *
 * `Checkerboard` is where the weak-consensus case lives, and it stopped being a leftover the moment
 * the photograph arrived: nothing in the hangar can produce the answered-but-not-accepted outcome
 * `Acceptance` exists to catch, so that test needs a world where consensus can be weak and this is
 * one.
 *
 * **How that was measured, since the instrument is not in the tree.** A temporary probe in this file
 * ran all eleven consecutive pairs of the twelve-frame hangar ring against all three detectors at
 * five perturbations of the prior — 1, 2, 3, 4 and 6 degrees about x — and printed
 * `inliers / correspondences` for each. 165 combinations, none refused, none unaccepted, lowest
 * inlier fraction 0.6522 against the 0.2 `kInlierFraction` accepts at. Nothing in the suite asserts that
 * floor, so it would not go red if the hangar one day *could* reach the weak case; ADR 0056 hit the
 * same problem and the answer is the same, which is to say so rather than let a reader assume a test
 * covers it.
 */
enum class World { Photograph, Checkerboard };

/** Relative to the repository root, which is where the generator's command runs. */
inline constexpr const char* kPhotograph = "core/test/data/panoramas/small_hangar_01_1k.jpg";

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
  /** Whether the world it was asked for is not in the tree, which is a failure and not a skip.
   *
   * `false` says two different things and the caller cannot tell them apart: for a world with an
   * input on disk it says the input is there, and for one this renderer generates it says the
   * question does not arise. Both are the answer a caller wants — proceed — which is why one value
   * carries them; a reader comparing the two arms of `World` should not have to work that out.
   *
   * **It answers "the file is not there", not "the world is usable".** A panorama that is present
   * and unreadable — truncated, an LFS pointer the smudge filter never expanded, or simply too
   * large for the decoder — leaves this `false`, fails the renderer instead, and *skips*. That is
   * deliberate: the broader check is "the renderer refused its input", which would be a second copy
   * of the generator's refusal taxonomy kept in step by hand here. `tools/gate.sh`'s `accuracy
   * measured` step and its CI twin already fail on `grep -q SKIPPED` and do not care which cause
   * produced the skip, so the guarantee lives there. This is the cheap tripwire for the one cause a
   * contributor actually creates, so that a partial local `ctest` says something useful.
   *
   * **The two `ASSERT_FALSE(rendered.inputMissing())` sites are kept in step by hand, and a third
   * world is when to stop doing that.** A helper would have to be a macro — `ASSERT_` expands to a
   * bare `return` — and a macro that hides a return behind a name is a worse trade than one
   * duplicated line. At three sites it stops being a pair and becomes a copy, and the `--panorama`
   * ternary in the command below becomes a second one at the same moment, since today both those
   * lines say `Photograph` only because the enum has two values.
   */
  bool inputMissing() const { return missingInput_; }

  Rendered(int frames, int edgeWidth, int edgeHeight, World world) {
    // **Checked here so a missing panorama fails rather than skips.** Everything else that stops
    // this rendering — no `uv`, no `datasets` group, no network for a first resolve — is a
    // contributor's bare checkout and is rightly a skip. A photograph that is not in the tree is
    // not that: it is the measurement's input gone, and a skip would be green on its own and
    // caught only by `tools/gate.sh` noticing the word SKIPPED.
    if (world == World::Photograph && !fs::exists(fs::path(RepoRoot()) / kPhotograph)) {
      missingInput_ = true;
      why_ = std::string(kPhotograph) + " is not in the tree, so there is no world to measure in";
      return;
    }
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
    // **The private directory holds the dataset *and* the log, and `--out` is a child of it.** The
    // generator swaps its output directory into place and deletes what was there, so a log written
    // into `--out` is a log the next run of the generator throws away. A child keeps both inside
    // the 0700 perimeter `mkdtemp` bought, which is the point: the parent is the world-writable
    // temp root, where a predictable name is something anyone on the machine can pre-create as a
    // symlink for `>` to follow and truncate.
    private_ = fs::path(buffer.data());
    path_ = private_ / "dataset";
    made_ = true;

    // **Its output is kept, not sent to `/dev/null`.** Every way this can fail used to arrive as
    // the same skip message — "`uv` and the `datasets` group are needed" — including a renderer
    // crash, a lock file that no longer resolves, and a checkout path that broke the shell. A skip
    // that misdiagnoses its own cause is worse than one that says nothing.
    const fs::path log = private_ / "renderer.log";
    const std::string command =
        "cd " + Quoted(RepoRoot()) +
        " && uv run --locked --group datasets tools/synth_dataset.py --out " +
        Quoted(path_.string()) + " --frames " + std::to_string(frames) + " --width " +
        std::to_string(edgeWidth) + " --height " + std::to_string(edgeHeight) +
        (world == World::Photograph ? " --panorama " + Quoted(kPhotograph) : "") + " >" +
        Quoted(log.string()) + " 2>&1";
    const int status = std::system(command.c_str());
    ok_ = status == 0 && fs::exists(path_ / "truth.json");
    if (!ok_) {
      // `std::system` answers with a *wait status*, so the raw number is the exit code shifted left
      // by eight — "the renderer exited 1792" for an exit of 7, which sends a reader looking for a
      // signal number that does not exist.
      const std::string code = WIFEXITED(status) ? std::to_string(WEXITSTATUS(status))
                                                 : "a signal, raw status " + std::to_string(status);
      why_ = "the renderer exited " + code + ": " + Tail(log);
    }
    std::error_code ignored;
    fs::remove(log, ignored);
  }
  ~Rendered() {
    if (!made_) return;
    std::error_code ignored;
    fs::remove_all(private_, ignored);
  }
  Rendered(const Rendered&) = delete;
  Rendered& operator=(const Rendered&) = delete;

  bool ok() const { return ok_; }
  std::string path() const { return path_.string(); }
  /** Why it did not render, for the skip message. Empty when it did. */
  const std::string& why() const { return why_; }

 private:
  /** The last few lines of the renderer's output, which is where its complaint is.
   *
   * Five lines held, not the whole log: the renderer's output is small today and is whatever it
   * decides to print tomorrow, and a ring buffer costs the same as a vector to write.
   */
  static std::string Tail(const fs::path& log) {
    std::ifstream stream(log);
    if (!stream) return "(no output was captured)";
    constexpr size_t kLines = 5;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(stream, line)) {
      if (lines.size() == kLines) lines.erase(lines.begin());
      lines.push_back(line);
    }
    if (lines.empty()) return "(it said nothing)";
    std::string tail;
    for (const std::string& kept : lines) {
      tail += kept;
      tail += "\n";
    }
    return tail;
  }

  fs::path private_;
  fs::path path_;
  std::string why_;
  bool ok_ = false;
  bool made_ = false;
  bool missingInput_ = false;
};

}  // namespace sphanorama::test
