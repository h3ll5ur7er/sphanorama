add_library(sphanorama_core
  src/resource_access/camera_access/null_camera_access.cpp
  src/resource_access/export_access/null_export_access.cpp
  src/resource_access/frame_store_access/memory_frame_store_access.cpp
  src/resource_access/motion_sensor_access/null_motion_sensor_access.cpp
  src/resource_access/project_store_access/memory_project_store_access.cpp
  src/managers/panorama_build_manager/panorama_build_manager.cpp
  src/managers/project_manager/project_manager.cpp
  src/managers/capture_session_manager/capture_session_manager.cpp
  src/engines/composition_engine/nearest_centre_composition_engine.cpp
  src/engines/composition_engine/null_composition_engine.cpp
  src/engines/coverage_planner_engine/null_coverage_planner_engine.cpp
  src/engines/coverage_planner_engine/rings_coverage_planner_engine.cpp
  src/engines/frame_preview_engine/box_frame_preview_engine.cpp
  src/engines/frame_quality_engine/null_frame_quality_engine.cpp
  src/engines/frame_quality_engine/sharpness_frame_quality_engine.cpp
  src/engines/pose_engine/null_pose_engine.cpp
  src/engines/pose_engine/orientation_pose_engine.cpp
  src/engines/registration_engine/null_registration_engine.cpp
  src/utilities/arena.cpp
  src/utilities/camera_model.cpp
  src/utilities/clock.cpp
  src/utilities/config_store.cpp
  src/utilities/diagnostics.cpp
  src/utilities/equirect.cpp
  src/utilities/event_bus.cpp
  src/utilities/logger.cpp
  src/utilities/pixel_format.cpp
  src/utilities/quaternion.cpp
  src/utilities/quaternion_average.cpp
  src/utilities/kept_lens.cpp
  src/utilities/rotation_averaging.cpp
  src/utilities/session_document.cpp
)

# The registration engine is the first shipped core code that depends on OpenCV (ADR 0052). It
# compiles wherever OpenCV does, which is every preset but `native-contracting` since ADR 0069, and a
# build without it gets `NullRegistrationEngine` — which is already in the sources above,
# unconditionally, because the null object is what the composition root falls back to rather than a
# stand-in for a missing file.
if(SPHANORAMA_WITH_OPENCV)
  target_compile_definitions(sphanorama_core PUBLIC SPHANORAMA_WITH_OPENCV=1)
endif()

target_include_directories(sphanorama_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(sphanorama_core PUBLIC sphanorama_contracts)

# Exceptions and RTTI are off in the core: nothing may throw across the WASM boundary (ADR 0006),
# and both cost binary size we have budgeted away. The registration engine below takes
# `-fexceptions` back, because `cv::Exception` is how OpenCV reports ordinary failure, and it
# converts at its own edge (ADR 0052).
#
# -fno-rtti is PUBLIC rather than PRIVATE because it changes what the compiler emits for a class,
# not just how this library is built. A consumer compiled with RTTI references type info the core
# never emitted, and the link fails — or worse, succeeds by luck under one build type and not
# another. Anything linking the core is built the same way.
target_compile_options(sphanorama_core PUBLIC -fno-rtti)
target_compile_options(sphanorama_core PRIVATE
  -fno-exceptions
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)

if(SPHANORAMA_WITH_OPENCV)
  # A library of its own so that what linking OpenCV costs travels with it. OpenCV throws, and under
  # WebAssembly a throw with no exception-handling runtime linked aborts the module rather than
  # reaching the engine's `catch` — and nothing warns at link time. So the link flag is this
  # library's interface: whatever links the engine links the runtime, and the module, which does
  # not link it yet, does not pay the 29 KB for it (ADR 0069).
  #
  # ADR 0047 named the exception question and left it to the ADR introducing the first
  # OpenCV-backed engine; ADR 0052 answered it: this translation unit may throw and must catch, and
  # nothing above it learns that OpenCV throws.
  add_library(sphanorama_registration
    src/engines/registration_engine/feature_registration_engine.cpp)
  target_link_libraries(sphanorama_registration PUBLIC sphanorama_core PRIVATE sphanorama_opencv)
  target_compile_options(sphanorama_registration PRIVATE
    -fexceptions
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
  if(EMSCRIPTEN)
    target_link_options(sphanorama_registration INTERFACE -fwasm-exceptions)
  endif()
endif()
