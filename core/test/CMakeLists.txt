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
  set(SPHANORAMA_OPENCV_TESTS
      "utilities/camera_model_opencv_test.cpp"
      # Registration exists only where OpenCV does (ADR 0052), so its tests do too. A reader looking
      # for registration coverage in a WASM-only checkout will find none, and that is the reason.
      "engines/registration_engine_test.cpp"
      # The synthetic-dataset loader reads `truth.json` with `cv::FileStorage`, and everything that
      # consumes it is on the registration path, so it lives behind the same flag rather than
      # dragging a second JSON reader into a build that has one.
      "support/synthetic_dataset.cpp"
      "support/synthetic_dataset_test.cpp")
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

  # The two translation units here that may throw, for the same reason `feature_registration_engine`
  # does in the core (ADR 0052): `cv::Exception` is how OpenCV reports ordinary failure, and the
  # loader converts it to a `Result` at its own edge. The test file takes it too — it copies and
  # deletes directories to build the damaged datasets its refusal cases need, and `std::filesystem`
  # reports those failures by throwing, which under `-fno-exceptions` is a terminate that reads as a
  # crash rather than as the fixture problem it is. Appended after the target's own options, so it
  # wins over the `-fno-exceptions` the core exports.
  set_source_files_properties(support/synthetic_dataset.cpp support/synthetic_dataset_test.cpp
    PROPERTIES COMPILE_OPTIONS "-fexceptions")

  # Where the committed dataset lives. A path rather than a copy into the build tree: the fixture is
  # read and never written, and a test that silently read a stale copy is the failure this whole
  # harness exists to avoid.
  target_compile_definitions(sphanorama_tests
    PRIVATE SPHANORAMA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

  # Its own binary, and the reason is the thing it does: it replaces global `operator new` so a
  # chosen allocation fails, which is not something to do to a process 700 other tests share. Nor
  # is it a gtest — gtest allocates while it runs, so the sweep's counter would move under it.
  #
  # What it buys is the one property no fake store can reach: a refusal *giving back* every frame it
  # allocated needs an
  # allocation to fail inside `std::vector`, not inside the store. Two windows were open in round 1
  # because nothing could ask that question, and three reviewers each built this instrument by hand
  # to find them. Keeping it costs one small executable.
  add_executable(sphanorama_dataset_alloc_test support/synthetic_dataset_alloc_test.cpp
                                               support/synthetic_dataset.cpp)
  target_include_directories(sphanorama_dataset_alloc_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(sphanorama_dataset_alloc_test PRIVATE sphanorama_core sphanorama_opencv)
  target_compile_options(sphanorama_dataset_alloc_test PRIVATE -Wall -Wextra -Werror)
  # Through the source property, not `target_compile_options`, and the difference is the trap this
  # file already documents twenty lines up: a target's own options are emitted *before* the
  # INTERFACE options it inherits, so a `-fexceptions` put there would read as
  # `-fexceptions … -fno-exceptions` and lose to the flag the core exports.
  # `synthetic_dataset.cpp` already carries the property from the call above — source file
  # properties are directory-scoped, so it is set once for both targets that compile it.
  set_source_files_properties(support/synthetic_dataset_alloc_test.cpp
    PROPERTIES COMPILE_OPTIONS "-fexceptions")
  target_compile_definitions(sphanorama_dataset_alloc_test
    PRIVATE SPHANORAMA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
  # Named for what the header promises. It was `DatasetRefusalAllocatesNothing`, after a sentence
  # the header has never contained — a refusal *does* allocate, and then gives the frames back,
  # which is a weaker claim and the true one. Five places quoted the invented version.
  add_test(NAME DatasetRefusalGivesEveryFrameBack COMMAND sphanorama_dataset_alloc_test)
  # Leak detection stays **on**, with one suppression whose reasoning is in the file it names. The
  # first version of this disabled leak detection for the whole binary, which is process-wide on the
  # only test that reaches the loader's exception arms — a real leak planted anywhere in it would
  # have been green. The narrow form is better for every leak that is not a frame, which is most of
  # this binary. It is **not** better for the case first cited here — "LeakSanitizer is what caught
  # this instrument's own first defect, a sweep that stayed armed through its own cleanup" — because
  # that defect leaks frames, and every frame's allocation stack runs through the suppressed
  # function. Round 7 withdrew the argument in two places and left it here.
  set_tests_properties(DatasetRefusalGivesEveryFrameBack PROPERTIES
    ENVIRONMENT
      "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/support/dataset_alloc_test.lsan-suppressions")
endif()
target_link_libraries(sphanorama_tests PRIVATE sphanorama_core sphanorama_bridge GTest::gtest_main)
target_compile_options(sphanorama_tests PRIVATE -Wall -Wextra -Werror)

gtest_discover_tests(sphanorama_tests)
