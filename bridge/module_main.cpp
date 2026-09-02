// Entry point for the WASM module. The ABI itself lives in module.cpp, which builds natively so
// the native suite can exercise it; this file exists only to give the Emscripten link target a
// translation unit of its own.
#include "module.h"

// Referencing the exports keeps them from being stripped before EMSCRIPTEN_KEEPALIVE applies.
extern "C" int32_t sph_module_link_anchor();
extern "C" int32_t sph_module_link_anchor() { return sph_probe_field_count(); }
