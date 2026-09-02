#include "capabilities.h"

namespace sphanorama::bridge {

RuntimeCapabilities ProbeRuntime(int32_t reportedConcurrency, bool crossOriginIsolated) {
  RuntimeCapabilities caps;

#if defined(__wasm_simd128__)
  caps.simd = true;
#endif

  // SharedArrayBuffer is gated on cross-origin isolation regardless of what the build supports,
  // so a threaded binary served without COOP/COEP still has to run serially. Reporting the build
  // flag here instead of the runtime state is how that becomes a crash on a phone rather than a
  // number in a capability struct.
#if defined(__EMSCRIPTEN_PTHREADS__)
  caps.sharedMemory = crossOriginIsolated;
  caps.threads = crossOriginIsolated && reportedConcurrency > 1;
#else
  (void)crossOriginIsolated;
  caps.threads = false;
  caps.sharedMemory = false;
#endif

  caps.hardwareConcurrency = caps.threads ? reportedConcurrency : 0;
  return caps;
}

}  // namespace sphanorama::bridge
