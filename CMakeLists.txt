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

# OpenCV is the dependency ADR 0005 chose, ADR 0047 wires in and ADR 0069 cross-compiles: on by
# default on every platform, so the browser's build has the same registration engine the native
# tests measure — which its runtime does not select yet. Off is still a configuration — a build
# without it gets `NullRegistrationEngine` — and the clang build that asks a narrow question about
# the core's own arithmetic turns it off.
option(SPHANORAMA_WITH_OPENCV "Build the OpenCV-backed engines and cross-checks" ON)
if(EMSCRIPTEN AND SPHANORAMA_WITH_OPENCV)
  # WebAssembly's own exception handling, for the one translation unit that catches OpenCV's
  # (ADR 0052) and for OpenCV itself, which throws. Emscripten's JavaScript-emulated exceptions are
  # the alternative, and ADR 0069 says why not. Compiled everywhere, which changes nothing in a
  # translation unit built `-fno-exceptions`; *linked* by whatever links the registration engine,
  # whose library carries the flag, because the link is what brings in an exception-handling
  # runtime — 28,961 bytes gzipped on a module that never throws.
  add_compile_options(-fwasm-exceptions)
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
# and every developer machine this could reasonably meet, and it reproduces the rounding split the
# option exists to expose (measured: with `Norm` written as a plain sum, seven quaternions the gate
# admits come back from `Normalize` as the identity under it). On aarch64 fused multiply-add is
# baseline and no flag is needed. `-ffp-contract=fast` is global on purpose, so the tests are
# compiled the way the code they check is.
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
if(EMSCRIPTEN AND SPHANORAMA_WITH_OPENCV)
  include("${CMAKE_CURRENT_SOURCE_DIR}/core/test/wasm_accuracy.cmake")
endif()

if(SPHANORAMA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(core/test)
endif()
