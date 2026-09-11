add_library(sphanorama_core
  src/resource_access/camera_access/null_camera_access.cpp
  src/resource_access/export_access/null_export_access.cpp
  src/resource_access/frame_store_access/memory_frame_store_access.cpp
  src/resource_access/motion_sensor_access/null_motion_sensor_access.cpp
  src/resource_access/project_store_access/memory_project_store_access.cpp
  src/managers/panorama_build_manager/panorama_build_manager.cpp
  src/managers/project_manager/project_manager.cpp
  src/managers/capture_session_manager/capture_session_manager.cpp
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
  src/utilities/event_bus.cpp
  src/utilities/logger.cpp
  src/utilities/pixel_format.cpp
  src/utilities/quaternion.cpp
)

# The registration engine is the first shipped core code that depends on OpenCV, and the WASM build
# does not have it (ADR 0047, ADR 0052). So it compiles only where OpenCV does, and a build without
# it gets `NullRegistrationEngine` — which is already in the sources above, unconditionally, because
# the null object is what the composition root falls back to rather than a stand-in for a missing
# file.
if(SPHANORAMA_WITH_OPENCV)
  target_sources(sphanorama_core
    PRIVATE src/engines/registration_engine/feature_registration_engine.cpp)
  # The one translation unit in the core that may throw, and the one that has to catch.
  #
  # ADR 0047 named this and left the decision to the ADR introducing the first OpenCV-backed engine:
  # `cv::Exception` is how OpenCV reports ordinary failure, so an engine calling it cannot be
  # compiled `-fno-exceptions` and still keep the `Result<T>` every contract here promises. ADR 0052
  # takes that decision — this file converts at its own edge and nothing above it learns that
  # OpenCV throws. `-fexceptions` is appended after the target's own options, so it wins.
  set_source_files_properties(src/engines/registration_engine/feature_registration_engine.cpp
    PROPERTIES COMPILE_OPTIONS "-fexceptions")
  target_link_libraries(sphanorama_core PRIVATE sphanorama_opencv)
  target_compile_definitions(sphanorama_core PUBLIC SPHANORAMA_WITH_OPENCV=1)
endif()

target_include_directories(sphanorama_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(sphanorama_core PUBLIC sphanorama_contracts)

# Exceptions and RTTI are off in the core: nothing may throw across the WASM boundary (ADR 0006),
# and both cost binary size we have budgeted away.
#
# -fno-rtti is PUBLIC rather than PRIVATE because it changes what the compiler emits for a class,
# not just how this library is built. A consumer compiled with RTTI references type info the core
# never emitted, and the link fails — or worse, succeeds by luck under one build type and not
# another. Anything linking the core is built the same way.
target_compile_options(sphanorama_core PUBLIC -fno-rtti)
target_compile_options(sphanorama_core PRIVATE
  -fno-exceptions
  -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror)
