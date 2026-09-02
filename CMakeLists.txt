cmake_minimum_required(VERSION 3.24)
project(sphanorama LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SPHANORAMA_BUILD_TESTS "Build the native test suite" ON)

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
