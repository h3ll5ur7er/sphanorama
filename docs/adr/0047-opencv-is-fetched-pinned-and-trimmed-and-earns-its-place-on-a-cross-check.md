# 0047 — OpenCV is fetched, pinned and trimmed, and earns its place on a cross-check

**Status:** accepted; one consequence superseded by
[ADR 0052](0052-opencv-enters-the-core-behind-a-build-flag.md)

> **The sanitizer remedy named below is superseded.** This ADR said that when OpenCV tripped the
> sanitizers the answer would be "a suppressions file scoped to `_deps/opencv-src`, not turning
> recovery back on". Those two cannot both hold: a UBSan suppressions file is only consulted for
> *recoverable* errors, so under this repository's `-fno-sanitize-recover=all` it never applies.
> Measured, after an earlier draft of this banner got the mechanism wrong twice: the file is read
> **lazily, at the first diagnostic**, not at startup. A clean binary with a malformed suppressions
> file exits 0 in silence; only a binary that trips UBSan reports `failed to parse suppressions`. So
> a stale or mistyped file sits in a green job indefinitely and announces itself on the day something
> else breaks — which is the opposite of the reassurance the previous wording offered.
> ADR 0052 records the measurement and takes a compile-time `-fno-sanitize=alignment` scoped to
> OpenCV's subdirectory instead, which reaches the goal this ADR actually wanted — OpenCV exempt,
> our own code strict, recovery still off everywhere.
>
> The exception-boundary shape this ADR named and left for a later ADR to build also stands, and
> 0052 built it as described.
>
> The bullet carrying the superseded remedy is stale throughout, not in two details, and saying "two
> smaller claims" undercounted it. Its headline — that instrumenting OpenCV was a bill that had not
> arrived — is exactly what this branch collected: 295 of 295 OpenCV translation units now carry
> `-fno-sanitize=alignment`, against 0 of ours. Its prediction was wrong about the mechanism too. It
> expected an unsigned overflow in `features2d`, which `-fsanitize=undefined` does not even enable;
> what fired was an alignment fault in `imgproc`, from a lookup-table gather in `cv::resize`. And the
> translation-unit count in it is 295 rather than 296. The reasoning that recovery should stay off
> is the part that survived, and it is why the remedy had to change rather than the policy.

## Context

ADR 0005 decided *which* OpenCV to depend on — `core`, `imgproc`, `features2d`, `calib3d`, `photo`
and `flann`, called piecemeal from behind our own contracts, with the `stitching` module deliberately
unused. That was written before there was anything to build against, and it stayed a plan: until now
the string "OpenCV" appeared in four documents and in no `CMakeLists.txt`. Phase 2 is where it stops
being a plan, and `RegistrationEngine` cannot be written without it.

Two things about this repository make the wiring less obvious than it looks.

**The core is compiled `-fno-rtti` and `-fno-exceptions`** (ADR 0006, ADR 0012). Nothing may throw
across the WASM boundary, and both features cost binary size that has been budgeted away. OpenCV is
the opposite on both counts: it throws `cv::Exception` from ordinary failures and its headers use
RTTI. `-fno-rtti` is `PUBLIC` on `sphanorama_core`, so it reaches anything that links the core.

**The WASM build is a separate problem.** A trimmed OpenCV cross-compiled to WASM has its own size
budget, its own SIMD questions, and its own failure modes, and the roadmap already defers it until
Phase 2 registration needs it in a browser. Nothing about writing the algorithms requires it first.

## Decision

**OpenCV is fetched from source at a pinned tag, built as a trimmed static subset, and linked
natively only.**

- `cmake/opencv.cmake` declares it with `FetchContent`, pinned to the commit
  `SPHANORAMA_OPENCV_COMMIT` (`71d3237`, the tip of 4.10.0), shallow, and verified after checkout.
  Same reasoning as the googletest pin: a floating dependency turns an unrelated upstream change
  into a red build on a day nobody touched this repo. A distribution package could not be pinned
  this way and could not be the same OpenCV the WASM build will eventually cross-compile.
- `SPHANORAMA_OPENCV_MODULES` is exactly ADR 0005's six modules, written once. `BUILD_LIST`, the
  include directories and the link libraries are all derived from it, so a module cannot be linked
  without being built or included without being linked. `BUILD_LIST` prunes the *configure* step,
  not just the link: the modules we do not name are never configured and never compiled. `dnn`,
  `highgui`, `imgcodecs`, `ml`, `objdetect`, `stitching`, `video` and `videoio` fall out by
  dependency.
- `SPHANORAMA_WITH_OPENCV` is `ON` for native builds and forced `OFF` under Emscripten.

**And its first use is a cross-check, not an engine.** `camera_model_opencv_test.cpp` checks our
Brown-Conrady against `cv::projectPoints`, and — separately — asks our `Unproject` to invert *their*
forward map.

## Consequences

- The claim that has been argued about for three review rounds — "Brown-Conrady in OpenCV's parameter
  convention, so a calibration drops in unchanged" — is now checked against the implementation it
  names, rather than against a human's arithmetic or against itself.
- **The inverse test is deliberately not an agreement test.** `cv::undistortPoints` runs five passes
  of the fixed-point iteration ADR 0046 replaced, so on a wide lens OpenCV's inverse is the one that
  is wrong. Asking our solver to invert their *forward* map is both stronger and true on lenses where
  agreeing with their inverse would be a defect. A 115° lens that OpenCV projects happily and its own
  inverse cannot solve is in the suite for exactly that reason.
- **Cost, honestly.** A cold configure downloads the tree and the first build is long — minutes, on
  four cores — and it lands in `tools/gate.sh`. It is cached in the build directory afterwards, and
  CI caches `_deps` **and `.ninja_log`**: the first version of that cache omitted the log, and since
  ninja marks any output with no log entry dirty, it stored 946 MB per entry and rebuilt all 244
  OpenCV edges regardless. A reviewer measured it. The entry is still large, most of it the `.git`
  directory a shallow clone leaves behind.

- **"Pinned" is weaker than it sounds, and this ADR overstated it — so the pin is now a commit.**
  The first version of this decision named a tag, and a tag is a mutable ref that resolves through
  the remote at fetch time with no hash to check the answer against, so the same name can serve
  different bytes. `SPHANORAMA_OPENCV_COMMIT` is that commit, and `SPHANORAMA_OPENCV_TAG` stays
  beside it as documentation, because a SHA alone does not say which release this is and a bump has
  to move both.

  Two things about how that is done are worth writing down, because both were measured rather than
  assumed. CMake documents `GIT_SHALLOW` as working "only with branch names and tags" — a commit
  hash "is not allowed" — and yet a probe configure against this commit produced a one-commit
  history at exactly this SHA against GitHub, with a 325 MB `.git` rather than OpenCV's full
  history. So the shallow clone is kept and the configure *verifies what it got*: it reads
  `git rev-parse HEAD` in the populated tree and fails if it is not the pinned commit. A pin nobody
  checks is a wish, and the reachability rules that make the shallow trick work are upstream's to
  change.

  What is still not bought is reproducibility: the build links the host's `libz.so.1`, so the
  artifact differs across machines even when the source does not. Vendoring zlib would close that,
  and it is not worth doing until something depends on it.
- **The sanitizer job now instruments all of OpenCV, and that is a bill that has not arrived yet.**
  `CMAKE_CXX_FLAGS` in the `native-asan` preset is global, so the sanitizers reach every translation
  unit in the build tree — 296 OpenCV ones, measured from `compile_commands.json`, all carrying
  `-fsanitize=address,undefined` under `-fno-sanitize-recover=all`. Today that is green, because
  nothing calls into OpenCV outside `camera_model_opencv_test.cpp` and `cv::projectPoints` is well
  behaved. The first `RegistrationEngine` call into `features2d` may not be: an unsigned overflow
  deep in a third-party SIMD path would abort the job with no suppression file to hold it, in code
  we did not write and will not fix. The remedy when that day comes is a suppressions file scoped to
  `_deps/opencv-src`, not turning recovery back on — recovery off is what makes this job worth
  running on our own code. Naming it now is cheaper than diagnosing it under a red build.
- **RTTI turned out not to be needed at all**, and the flag that said otherwise never ran. This ADR
  first claimed the test target takes `-fno-rtti` back off because OpenCV's headers need RTTI. Both
  halves were wrong: CMake emits a target's own `COMPILE_OPTIONS` before the `INTERFACE` options it
  inherits, so the command line read `-frtti … -fno-rtti` and the inherited flag won — and it
  compiled anyway, which is the useful fact. The subset depended on here does not need RTTI. The flag
  is gone; nothing is being worked around.
- **The exception boundary is named here and not yet built.** When an engine calls OpenCV it cannot
  be compiled `-fno-exceptions`, because `cv::Exception` is how OpenCV reports ordinary failure. The
  shape that fits: the OpenCV-backed implementation is its own component, compiled with exceptions,
  catching at its own edge and returning `Result<T>` — which is what the layer rules already ask of a
  component that adapts something foreign. That is a decision for the ADR that introduces the first
  such engine, and writing it down now is cheaper than discovering it then.

## Rejected

***A system package (`apt install libopencv-dev`).*** Faster to configure and impossible to pin. The
version would differ between a developer's machine, CI and whatever the WASM build eventually
compiles, which is three answers to a question that has to have one — and the modules would be
whatever the distribution chose rather than the six ADR 0005 argued for.

***Waiting until `RegistrationEngine` needs it.*** Tempting, and it would have avoided a long build
in this change. But it would have meant writing the registration engine and the dependency wiring in
one commit, with the size budget, the RTTI flag, the exception boundary and the algorithm all failing
for each other's reasons. Landing the dependency against a test we can already state the right answer
for separates those failures.

***Cross-compiling to WASM in the same change.*** The browser needs this eventually and does not need
it to write the algorithms. Doing it now would put a size-budget negotiation in front of every
Phase 2 commit, and the roadmap already sequences it after.
