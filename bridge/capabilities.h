#pragma once

#include <cstdint>

// The C++ side of the WASM boundary. This is the ONLY tree allowed to reference Emscripten:
// tools/no_browser_check.py fails the build if browser assumptions appear anywhere in core/.
//
// Layer-wise this is a client: it calls managers and nothing else.
namespace sphanorama::bridge {

// What the runtime can actually do, probed rather than assumed. Threads require cross-origin
// isolation (COOP/COEP), which some hosts — GitHub Pages among them — cannot serve, so a
// thread count of zero is a supported configuration and not a degraded one.
struct RuntimeCapabilities {
  bool simd = false;
  bool threads = false;
  bool sharedMemory = false;
  int32_t hardwareConcurrency = 0;
};

RuntimeCapabilities ProbeRuntime(int32_t reportedConcurrency, bool crossOriginIsolated);

}  // namespace sphanorama::bridge
