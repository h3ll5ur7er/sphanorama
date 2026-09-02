add_library(sphanorama_core
  src/utilities/arena.cpp
  src/utilities/clock.cpp
  src/utilities/config_store.cpp
  src/utilities/diagnostics.cpp
  src/utilities/event_bus.cpp
  src/utilities/logger.cpp
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
