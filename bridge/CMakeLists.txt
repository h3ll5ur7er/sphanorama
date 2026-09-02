# The C++ side of the WASM boundary. Builds natively too — that is not a curiosity, it is how
# the capability probe and (later) the facade's marshalling get tested without a browser.
add_library(sphanorama_bridge capabilities.cpp facade.generated.cpp module.cpp runtime.cpp)

target_include_directories(sphanorama_bridge PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(sphanorama_bridge PUBLIC sphanorama_core)
target_compile_options(sphanorama_bridge PRIVATE
  -fno-exceptions -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)

if(EMSCRIPTEN)
  # The module clients load. Two variants are produced by the two wasm presets: a threaded one
  # for hosts that can serve COOP/COEP, and a single-threaded one for hosts that cannot
  # (ADR 0011). Both must be correct; neither is a fallback.
  add_executable(sphanorama_module module_main.cpp)
  target_link_libraries(sphanorama_module PRIVATE sphanorama_bridge)
  target_link_options(sphanorama_module PRIVATE
    -sMODULARIZE=1
    -sEXPORT_ES6=1
    -sEXPORT_NAME=createSphanoramaCore
    -sALLOW_MEMORY_GROWTH=1
    -sENVIRONMENT=web,worker
    -sFILESYSTEM=0
    -sEXPORTED_FUNCTIONS=_sph_probe_runtime,_sph_probe_field_count,_sph_probe_field_name,_sph_facade_call,_sph_facade_method_count,_sph_facade_method_name,_sph_facade_result,_malloc,_free
    -sEXPORTED_RUNTIME_METHODS=HEAP32,HEAPU8,UTF8ToString
  )
  set_target_properties(sphanorama_module PROPERTIES OUTPUT_NAME "sphanorama-core" SUFFIX ".js")
endif()
