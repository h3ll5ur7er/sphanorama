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
