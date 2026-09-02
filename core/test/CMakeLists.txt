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

add_executable(sphanorama_tests
  managers/capture_session_manager_test.cpp
  managers/project_manager_test.cpp
  ${CMAKE_SOURCE_DIR}/bridge/test/capabilities_test.cpp
  resource_access/camera_access_contract_test.cpp
  resource_access/codec_and_export_test.cpp
  resource_access/frame_store_access_contract_test.cpp
  resource_access/motion_sensor_access_contract_test.cpp
  resource_access/project_store_access_contract_test.cpp
  support/fake_camera_access.cpp
  support/fake_export_access.cpp
  support/fake_frame_store_access.cpp
  support/fake_image_codec_access.cpp
  support/fake_motion_sensor_access.cpp
  support/fake_project_store_access.cpp
  utilities/arena_test.cpp
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
target_link_libraries(sphanorama_tests PRIVATE sphanorama_core sphanorama_bridge GTest::gtest_main)
target_compile_options(sphanorama_tests PRIVATE -Wall -Wextra -Werror)

gtest_discover_tests(sphanorama_tests)
