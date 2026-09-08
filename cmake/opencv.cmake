# OpenCV, fetched and built from source as a trimmed static subset.
#
# ADR 0005 decided to depend on OpenCV's algorithms piecemeal rather than on its `stitching` module,
# and named the parts: core, imgproc, features2d, calib3d, photo, flann. This is that list, and
# nothing else — `BUILD_LIST` prunes the configure step itself, so the modules we do not name are
# never configured, never compiled, and never linked.
#
# **From source, pinned, rather than from the system.** The same reasoning as googletest in
# core/test/CMakeLists.txt: a floating dependency turns an unrelated upstream change into a red build
# on a day nobody touched this repo. It also has to be the *same* OpenCV that the WASM build will
# eventually cross-compile, and a distribution package cannot be that.
#
# The cost is honest and worth stating: a cold configure downloads the tree and the first build is
# long. It is cached in the build directory afterwards.

include(FetchContent)

set(SPHANORAMA_OPENCV_TAG "4.10.0" CACHE STRING "OpenCV release tag to build against")

# Everything below is set before the subdirectory is added, because OpenCV reads these at configure
# time. FORCE because OpenCV's own CMakeLists caches many of them with defaults of its own.
set(BUILD_LIST "core,imgproc,features2d,calib3d,photo,flann" CACHE STRING "" FORCE)
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

FetchContent_Declare(opencv
  GIT_REPOSITORY https://github.com/opencv/opencv.git
  GIT_TAG        ${SPHANORAMA_OPENCV_TAG}
  GIT_SHALLOW    TRUE
  GIT_PROGRESS   TRUE
)
FetchContent_MakeAvailable(opencv)

# OpenCV's targets do not carry usable include directories when consumed from a build tree, so an
# interface target collects them once and everything that needs OpenCV links this instead.
add_library(sphanorama_opencv INTERFACE)
target_include_directories(sphanorama_opencv SYSTEM INTERFACE
  "${OPENCV_CONFIG_FILE_INCLUDE_DIR}"
  "${opencv_SOURCE_DIR}/include"
  "${opencv_SOURCE_DIR}/modules/core/include"
  "${opencv_SOURCE_DIR}/modules/imgproc/include"
  "${opencv_SOURCE_DIR}/modules/features2d/include"
  "${opencv_SOURCE_DIR}/modules/calib3d/include"
  "${opencv_SOURCE_DIR}/modules/photo/include"
  "${opencv_SOURCE_DIR}/modules/flann/include")
target_link_libraries(sphanorama_opencv INTERFACE
  opencv_core opencv_imgproc opencv_features2d opencv_calib3d opencv_photo opencv_flann)
