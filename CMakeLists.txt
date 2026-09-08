cmake_minimum_required(VERSION 3.24)
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
option(SPHANORAMA_WITH_OPENCV "Build the OpenCV-backed engines and cross-checks" ON)
if(EMSCRIPTEN)
  set(SPHANORAMA_WITH_OPENCV OFF)
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
