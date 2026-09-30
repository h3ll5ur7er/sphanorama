# The registration table measured in the browser's build (ADR 0069): the one test that runs in
# WebAssembly, and only where that build has OpenCV. Outside `bridge/`, so the size budget never
# counts it, and outside `core/test/CMakeLists.txt`, which the WebAssembly presets do not reach.
#
# `.cjs` because the repository's `package.json` says `"type": "module"`, and node would otherwise
# read the CommonJS loader the toolchain writes as a module and stop at its first `require`.
add_executable(sphanorama_wasm_accuracy
  ${CMAKE_CURRENT_LIST_DIR}/engines/registration_accuracy_wasm.cpp
  ${CMAKE_CURRENT_LIST_DIR}/support/synthetic_dataset.cpp
  ${CMAKE_CURRENT_LIST_DIR}/support/rotation_scoring.cpp)
target_include_directories(sphanorama_wasm_accuracy PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_link_libraries(sphanorama_wasm_accuracy PRIVATE sphanorama_core sphanorama_opencv)
# The test suite's warnings rather than the core's: it compiles the same test-support sources.
target_compile_options(sphanorama_wasm_accuracy PRIVATE -Wall -Wextra -Werror)
# The dataset is read from the host's disk, which is all a harness under node needs.
target_link_options(sphanorama_wasm_accuracy PRIVATE
  -sNODERAWFS=1 -sALLOW_MEMORY_GROWTH=1 -sENVIRONMENT=node -fwasm-exceptions)
set_target_properties(sphanorama_wasm_accuracy PROPERTIES SUFFIX ".cjs")
