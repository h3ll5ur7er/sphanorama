# 0047 — OpenCV is fetched, pinned and trimmed, and earns its place on a cross-check

**Status:** accepted

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

- `cmake/opencv.cmake` declares it with `FetchContent`, pinned to `SPHANORAMA_OPENCV_TAG` (4.10.0),
  shallow. Same reasoning as the googletest pin: a floating dependency turns an unrelated upstream
  change into a red build on a day nobody touched this repo. A distribution package could not be
  pinned this way and could not be the same OpenCV the WASM build will eventually cross-compile.
- `BUILD_LIST` is exactly ADR 0005's six modules. That prunes the *configure* step, not just the
  link: the modules we do not name are never configured and never compiled. `dnn`, `highgui`,
  `imgcodecs`, `ml`, `objdetect`, `stitching`, `video` and `videoio` fall out by dependency.
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
  CI will need a cache entry keyed on the tag.
- **RTTI is taken back off for the test target only.** `target_compile_options(sphanorama_tests
  PRIVATE -frtti)`, because OpenCV's headers need it and the core exports `-fno-rtti` publicly.
  Nothing in `core/src` includes an OpenCV header, so the core's own compilation is unchanged.
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
