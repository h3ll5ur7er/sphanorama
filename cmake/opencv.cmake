# OpenCV, fetched and built from source as a trimmed static subset.
#
# ADR 0005 decided to depend on OpenCV's algorithms piecemeal rather than on its `stitching` module,
# and named the parts: core, imgproc, features2d, calib3d, photo, flann. `SPHANORAMA_OPENCV_MODULES`
# below is that list, written once — `BUILD_LIST` prunes the configure step itself, so the modules we
# do not name are never configured, never compiled, and never linked, and the include directories and
# link libraries are derived from the same list rather than restated beside it.
#
# **From source, pinned, rather than from the system.** The same reasoning as googletest in
# core/test/CMakeLists.txt: a floating dependency turns an unrelated upstream change into a red build
# on a day nobody touched this repo. It also has to be the *same* OpenCV that the WASM build will
# eventually cross-compile, and a distribution package cannot be that.
#
# The cost is honest and worth stating: a cold configure downloads the tree and the first build is
# long. It is cached in the build directory afterwards.

include(FetchContent)

# Explicit rather than inherited: FetchContent finds git for its own use, but the pin check below
# depends on GIT_EXECUTABLE being set, and a variable that happens to be there is not a dependency.
find_package(Git REQUIRED)

# The commit, not the tag. A tag is a mutable ref that resolves through the remote at fetch time with
# no hash to check the result against, so the same name can serve different bytes; a SHA cannot. The
# tag is kept beside it because the SHA alone says nothing about which release this is, and because
# a bump has to move both.
set(SPHANORAMA_OPENCV_TAG "4.10.0"
    CACHE STRING "OpenCV release this build tracks — documentation for the SHA below")
set(SPHANORAMA_OPENCV_COMMIT "71d3237a093b60a27601c20e9ee6c3e52154e8b1"
    CACHE STRING "Exact OpenCV commit to build against (must be the tip of SPHANORAMA_OPENCV_TAG)")

# ADR 0005's six modules, in one place. Everything below derives from this.
set(SPHANORAMA_OPENCV_MODULES core imgproc features2d calib3d photo flann)

# Everything below is set before the subdirectory is added, because OpenCV reads these at configure
# time. FORCE because OpenCV's own CMakeLists caches many of them with defaults of its own.
string(REPLACE ";" "," SPHANORAMA_OPENCV_BUILD_LIST "${SPHANORAMA_OPENCV_MODULES}")
set(BUILD_LIST "${SPHANORAMA_OPENCV_BUILD_LIST}" CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(OPENCV_FORCE_3RDPARTY_BUILD OFF CACHE BOOL "" FORCE)

foreach(off
    BUILD_TESTS BUILD_PERF_TESTS BUILD_EXAMPLES BUILD_DOCS BUILD_opencv_apps
    BUILD_JAVA BUILD_opencv_java_bindings_generator BUILD_opencv_python_bindings_generator
    BUILD_opencv_python_tests BUILD_opencv_js_bindings_generator BUILD_opencv_objc_bindings_generator
    BUILD_PACKAGE BUILD_ITT BUILD_PROTOBUF
    OPENCV_GENERATE_PKGCONFIG OPENCV_GENERATE_SETUPVARS
    WITH_1394 WITH_ADE WITH_EIGEN WITH_FFMPEG WITH_GSTREAMER WITH_GTK WITH_IPP WITH_ITT
    WITH_JASPER WITH_JPEG WITH_LAPACK WITH_OPENCL WITH_OPENEXR WITH_OPENGL WITH_OPENJPEG
    WITH_OPENVX WITH_PNG WITH_PROTOBUF WITH_QUIRC WITH_TBB WITH_TIFF WITH_V4L WITH_WEBP
    WITH_IMGCODEC_HDR WITH_IMGCODEC_SUNRASTER WITH_IMGCODEC_PXM WITH_IMGCODEC_PFM)
  set(${off} OFF CACHE BOOL "" FORCE)
endforeach()

# `GIT_SHALLOW` with a commit hash is documented as unsupported — ExternalProject's docs say
# GIT_TAG "works only with branch names and tags" under it, because the clone is
# `--depth 1 --no-single-branch` and a hash is then only reachable by luck. It is kept anyway
# because it was measured rather than assumed: a probe configure against this commit produced a
# one-commit history at exactly this SHA and a 325 MB `.git` instead of OpenCV's full history. The
# check below is what turns "it stopped working" into a red configure rather than wrong bytes.
FetchContent_Declare(opencv
  GIT_REPOSITORY https://github.com/opencv/opencv.git
  GIT_TAG        ${SPHANORAMA_OPENCV_COMMIT}
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
)
# OpenCV's SIMD lookup-table gathers are exempt from the alignment check, and nothing else is.
#
# `hlineResizeCn` — the bit-exact 8-bit linear resize that `cv::ORB::detectAndCompute` uses to build
# its pyramid — reads a pair of source pixels through `v_lut_pairs(const uchar* tab, const int* idx)`,
# which does `*(const short*)(tab + idx[k])`. `idx[k]` is a *pixel* index into an 8-bit row, so that
# address is odd for half of all inputs no matter how the row's base is aligned: this is not a
# misalignment we could fix by allocating differently, and the read is in bounds — ASan reports
# nothing, only `-fsanitize=alignment` does. Undefined by the letter of the standard, and fine on the
# one instruction set this was measured on — x86-64, where unaligned loads are architectural. That is
# a statement about where we run the sanitizers, not a promise about every target: the WASM
# cross-compile ADR 0047 defers has its own answer to give when it arrives, and this comment is not
# it.
#
# It is scoped by saving and restoring CMAKE_CXX_FLAGS around the `add_subdirectory` that
# FetchContent performs, so the exemption reaches OpenCV's translation units and stops there. Our own
# code keeps the check, which matters: the flag is appended rather than the check dropped from the
# preset precisely so that a misaligned load *we* write still aborts the run — the check is a runtime
# one, so what it fails is the sanitizer job, not the compile. The alignment check is the only one
# lifted; ASan, and every other UBSan check, still cover OpenCV.
#
# It is appended unconditionally rather than only under the sanitizer preset, so there is one
# rule instead of a branch: OpenCV is always compiled with this check off, and in a tree with no
# sanitizers on — `native-debug` — the flag simply does nothing.
set(SPHANORAMA_CXX_FLAGS_BEFORE_OPENCV "${CMAKE_CXX_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-sanitize=alignment")
FetchContent_MakeAvailable(opencv)
set(CMAKE_CXX_FLAGS "${SPHANORAMA_CXX_FLAGS_BEFORE_OPENCV}")

# What was actually checked out, rather than what was asked for. A pin nobody verifies is a wish:
# the SHA above closes the mutable-ref hole only if the tree on disk is that commit, and a shallow
# clone whose reachability rules change upstream would otherwise hand us a different one silently.
execute_process(
  COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
  WORKING_DIRECTORY "${opencv_SOURCE_DIR}"
  OUTPUT_VARIABLE SPHANORAMA_OPENCV_HEAD
  OUTPUT_STRIP_TRAILING_WHITESPACE
  RESULT_VARIABLE SPHANORAMA_OPENCV_HEAD_STATUS
  ERROR_QUIET)
if(NOT SPHANORAMA_OPENCV_HEAD_STATUS EQUAL 0)
  message(FATAL_ERROR
    "Could not read the OpenCV checkout's commit in ${opencv_SOURCE_DIR}. The pin cannot be "
    "verified, so the build stops rather than compiling an unidentified tree.")
elseif(NOT SPHANORAMA_OPENCV_HEAD STREQUAL SPHANORAMA_OPENCV_COMMIT)
  message(FATAL_ERROR
    "OpenCV checkout is ${SPHANORAMA_OPENCV_HEAD}, expected ${SPHANORAMA_OPENCV_COMMIT} "
    "(${SPHANORAMA_OPENCV_TAG}). Delete the _deps directory if this is a stale tree; otherwise the "
    "pin no longer describes what is being built.")
endif()

# OpenCV's targets do not carry usable include directories when consumed from a build tree, so an
# interface target collects them once and everything that needs OpenCV links this instead. Both lists
# come from SPHANORAMA_OPENCV_MODULES: a module added there is included and linked without a second
# edit, and one removed cannot be left behind in a list nobody thought to look at.
add_library(sphanorama_opencv INTERFACE)
target_include_directories(sphanorama_opencv SYSTEM INTERFACE
  "${OPENCV_CONFIG_FILE_INCLUDE_DIR}"
  "${opencv_SOURCE_DIR}/include")
foreach(module IN LISTS SPHANORAMA_OPENCV_MODULES)
  target_include_directories(sphanorama_opencv SYSTEM INTERFACE
    "${opencv_SOURCE_DIR}/modules/${module}/include")
  target_link_libraries(sphanorama_opencv INTERFACE opencv_${module})
endforeach()
