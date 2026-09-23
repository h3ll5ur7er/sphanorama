# 3.25 rather than 3.24 because `CMakePresets.json` declares `"version": 6`, which CMake's own
# manual dates to 3.25 — so 3.24 could configure this project by hand and not by preset, which is
# the only way anything here is built.
cmake_minimum_required(VERSION 3.25)
project(sphanorama LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SPHANORAMA_BUILD_TESTS "Build the native test suite" ON)

# OpenCV is the dependency ADR 0005 chose and ADR 0047 wires in. It is off under Emscripten because
# cross-compiling the trimmed subset to WASM is its own piece of work with its own size budget — the
# roadmap defers it until Phase 2 registration needs it in a browser, and a native build that has it
# is what lets the algorithms be written and measured before then.
# Defaulted rather than overridden. The first version of this option()'d ON unconditionally and then
# used a plain set() to turn it off under Emscripten — which creates a *normal* variable shadowing the
# cached one, so `cmake -LA` reported ON for a wasm build and an explicit -DSPHANORAMA_WITH_OPENCV=ON
# was discarded without a word. A reviewer found it. Defaulting by platform keeps the cache honest,
# and asking for the impossible is now an error rather than a silence: someone who passes that flag
# under Emscripten has a reason, and saying nothing is the wrong answer to it.
if(EMSCRIPTEN)
  set(SPHANORAMA_WITH_OPENCV_DEFAULT OFF)
else()
  set(SPHANORAMA_WITH_OPENCV_DEFAULT ON)
endif()
option(SPHANORAMA_WITH_OPENCV "Build the OpenCV-backed engines and cross-checks"
       ${SPHANORAMA_WITH_OPENCV_DEFAULT})
if(EMSCRIPTEN AND SPHANORAMA_WITH_OPENCV)
  # The message names the stale cache first because that is how this actually fires. The option
  # defaulted ON unconditionally until the fix above, so any wasm build directory configured before
  # it still has ON written in its cache — where the new default cannot reach it, since a cached
  # value wins over the default forever. Telling that person to "build it natively" would send them
  # to the one place the problem is not.
  message(FATAL_ERROR
    "SPHANORAMA_WITH_OPENCV=ON is not available under Emscripten: the trimmed subset is not "
    "cross-compiled yet (ADR 0047).\n"
    "If you did not ask for this, the value is stale in ${CMAKE_BINARY_DIR}/CMakeCache.txt from a "
    "configure that predates the platform default. Delete that build directory and configure "
    "again.\n"
    "If you did ask for it: build natively instead — that is where the OpenCV cross-checks run.")
endif()
if(SPHANORAMA_WITH_OPENCV)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/opencv.cmake")
endif()

# The build on which two inlined copies of one sum of squares can fuse differently and land on
# opposite sides of a threshold — which the -O0 presets cannot show, because there nothing is
# inlined and every evaluation is the one compiled copy. `native-contracting` turns this on.
#
# An option rather than raw flags in the preset, because the instruction-set half is the host's
# business and the preset cannot ask which host it is on. On x86-64 fused multiply-add needs asking
# for, and it is asked for as a fixed level rather than `-march=native`: the GitHub runner's CPU
# carries AVX10, clang 18 promotes `avx10.1-256` to `-512` with a warning, and `-Werror` made that
# warning the whole job's failure on the first push. `x86-64-v3` is AVX2 plus FMA on every runner
# and every developer machine this could reasonably meet, and it reproduces the gate/divisor split
# the option exists to expose (measured: the round-9 shape fails the divisor witness under it
# exactly as under `-march=native`). On aarch64 fused multiply-add is baseline and no flag is needed.
# `-ffp-contract=fast` is global on purpose — the tests' own inlined copies are part of what has to
# fuse for the witness search to see anything.
option(SPHANORAMA_FUSED_ARITHMETIC
       "Contract floating-point multiply-adds across statements, on a target that can fuse them" OFF)
if(SPHANORAMA_FUSED_ARITHMETIC)
  add_compile_options(-ffp-contract=fast)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
    add_compile_options(-march=x86-64-v3)
  endif()
endif()

# The contracts directory is the include root rather than being mirrored into core/include:
# two copies of an interface is exactly the drift problem the contract-check exists to prevent.
# See docs/adr/0008-contracts-are-the-include-path.md.
add_library(sphanorama_contracts INTERFACE)
target_include_directories(sphanorama_contracts INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/contracts/cpp")

add_subdirectory(core)
add_subdirectory(bridge)

if(SPHANORAMA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(core/test)
endif()
