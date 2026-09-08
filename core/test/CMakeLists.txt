include(FetchContent)

# Pinned. A floating dependency turns an unrelated upstream change into a red build on a day
# nobody touched this repo.
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
  GIT_SHALLOW    TRUE
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# The OpenCV cross-check only exists when OpenCV does. Named through a variable rather than an
# if() around add_executable, so there is one source list rather than two that drift.
set(SPHANORAMA_OPENCV_TESTS "")
if(SPHANORAMA_WITH_OPENCV)
  set(SPHANORAMA_OPENCV_TESTS "utilities/camera_model_opencv_test.cpp")
endif()

add_executable(sphanorama_tests
  engines/coverage_planner_engine_test.cpp
  engines/frame_preview_engine_test.cpp
  engines/frame_quality_engine_test.cpp
  engines/pose_engine_test.cpp
  codec_test.cpp
  wire_test.cpp
  managers/capture_session_manager_test.cpp
  managers/project_manager_test.cpp
  ${CMAKE_SOURCE_DIR}/bridge/test/capabilities_test.cpp
  ${CMAKE_SOURCE_DIR}/bridge/test/heap_budget_test.cpp
  ${CMAKE_SOURCE_DIR}/bridge/test/facade_test.cpp
  resource_access/camera_access_contract_test.cpp
  resource_access/codec_and_export_test.cpp
  resource_access/frame_store_access_contract_test.cpp
  resource_access/frame_store_spill_test.cpp
  resource_access/motion_sensor_access_contract_test.cpp
  resource_access/project_store_access_contract_test.cpp
  support/fake_camera_access.cpp
  support/rotation_scoring.cpp
  support/rotation_scoring_test.cpp
  support/fake_spill_sink.cpp
  support/fake_export_access.cpp
  support/fake_image_codec_access.cpp
  support/fake_motion_sensor_access.cpp
  support/fake_project_store_access.cpp
  utilities/arena_test.cpp
  utilities/camera_model_test.cpp
  ${SPHANORAMA_OPENCV_TESTS}
  utilities/clock_test.cpp
  utilities/config_store_test.cpp
  utilities/diagnostics_test.cpp
  utilities/event_bus_test.cpp
  utilities/logger_test.cpp
  utilities/pixel_format_test.cpp
  utilities/quaternion_test.cpp
  utilities/result_test.cpp
)
target_include_directories(sphanorama_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/bridge")
if(SPHANORAMA_WITH_OPENCV)
  # No -frtti here, and the absence is the interesting part. This target used to carry one, on the
  # theory that OpenCV's headers need RTTI and the core exports -fno-rtti PUBLIC (ADR 0012). Both
  # halves were wrong. CMake emits a target's own COMPILE_OPTIONS *before* the INTERFACE options it
  # inherits, so the command line read `-frtti … -fno-rtti` and the inherited flag won — the flag
  # never did anything. And it compiled anyway, which is the part that matters: the subset of OpenCV
  # this depends on does not need RTTI, so nothing is being worked around.
  #
  # A reviewer found it by reading compile_commands.json rather than the CMakeLists, which is the
  # only place the answer was visible.
  target_link_libraries(sphanorama_tests PRIVATE sphanorama_opencv)
endif()
target_link_libraries(sphanorama_tests PRIVATE sphanorama_core sphanorama_bridge GTest::gtest_main)
target_compile_options(sphanorama_tests PRIVATE -Wall -Wextra -Werror)

gtest_discover_tests(sphanorama_tests)
