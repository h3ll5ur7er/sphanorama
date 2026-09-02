#pragma once

// Keeps an export from being stripped in the WASM build, and expands to nothing natively so the
// same sources compile for the bench and the test suite.
#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define SPH_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#define SPH_EXPORT
#endif
