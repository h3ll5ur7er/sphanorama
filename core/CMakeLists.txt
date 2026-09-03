add_library(sphanorama_core
  src/resource_access/camera_access/null_camera_access.cpp
  src/resource_access/export_access/null_export_access.cpp
  src/resource_access/frame_store_access/memory_frame_store_access.cpp
  src/resource_access/frame_store_access/null_frame_store_access.cpp
  src/resource_access/motion_sensor_access/null_motion_sensor_access.cpp
  src/resource_access/project_store_access/memory_project_store_access.cpp
  src/managers/panorama_build_manager/panorama_build_manager.cpp
  src/managers/project_manager/project_manager.cpp
  src/managers/capture_session_manager/capture_session_manager.cpp
  src/engines/composition_engine/null_composition_engine.cpp
  src/engines/coverage_planner_engine/null_coverage_planner_engine.cpp
  src/engines/coverage_planner_engine/rings_coverage_planner_engine.cpp
  src/engines/frame_quality_engine/null_frame_quality_engine.cpp
  src/engines/pose_engine/null_pose_engine.cpp
  src/engines/pose_engine/orientation_pose_engine.cpp
  src/engines/registration_engine/null_registration_engine.cpp
  src/utilities/arena.cpp
  src/utilities/clock.cpp
  src/utilities/config_store.cpp
  src/utilities/diagnostics.cpp
  src/utilities/event_bus.cpp
  src/utilities/logger.cpp
  src/utilities/pixel_format.cpp
  src/utilities/quaternion.cpp
)

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
