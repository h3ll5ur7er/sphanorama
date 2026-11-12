# stb's image writer and reader, fetched at a pinned commit and checked by content (ADR 0087).
#
# Two single headers of the many the repository carries: `stb_image_write.h` encodes the exported
# panorama as a JPEG inside the core, where the browser has no synchronous encoder to call, and
# `stb_image.h` decodes one back in the tests, so they check the file rather than the call.
#
# **By git at a commit, then by hash.** The commit is not a branch tip, so the clone is full rather
# than shallow — fourteen megabytes, once, cached in the build directory. The commit pins what was
# asked for; the SHA-256 of each header pins what arrived, which is the check a mutable ref or a
# rewritten history cannot pass.

include(FetchContent)

set(SPHANORAMA_STB_COMMIT "fede005abaf93d9d7f3a679d1999b2db341b360f"
    CACHE STRING "Exact stb commit to build against")

FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        ${SPHANORAMA_STB_COMMIT}
  GIT_SHALLOW    FALSE
)
# stb has no CMakeLists, so making it available only populates it.
FetchContent_MakeAvailable(stb)

foreach(pair
    "stb_image_write.h=cbd5f0ad7a9cf4468affb36354a1d2338034f2c12473cf1a8e32053cb6914a05"
    "stb_image.h=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3")
  string(REPLACE "=" ";" parts "${pair}")
  list(GET parts 0 header)
  list(GET parts 1 expected)
  file(SHA256 "${stb_SOURCE_DIR}/${header}" actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR
      "${header} in ${stb_SOURCE_DIR} is not the file this build pins: SHA-256 ${actual}, "
      "expected ${expected}. A bump moves SPHANORAMA_STB_COMMIT and both digests together.")
  endif()
endforeach()

# Header-only. SYSTEM, so its own code is held to its own warnings rather than to our -Werror.
add_library(sphanorama_stb INTERFACE)
target_include_directories(sphanorama_stb SYSTEM INTERFACE "${stb_SOURCE_DIR}")
