// The C ABI the JavaScript side calls.
//
// Deliberately not Embind. Two reasons, and the second is the load-bearing one:
//
//   1. Embind's type registry wants RTTI, which the core builds without (ADR 0008). The
//      -fno-rtti escape hatch compiles but produced an unbound type at runtime in this project,
//      where the crash is an out-of-bounds read inside Embind's own error path.
//   2. The boundary marshals through the shared heap (docs/04 §4.6) — frames already cross as
//      offsets, and structured calls will cross as FlatBuffers. Embind value-objects would be a
//      second, hand-written marshalling path alongside the generated one.
//
// So: plain C entry points writing into caller-provided heap offsets. See ADR 0012.
#include <emscripten/emscripten.h>

#include <cstdint>

#include "capabilities.h"

namespace {

// Field order is the contract. It is mirrored by the TypeScript loader, and a reordering here
// without one there would read plausible-looking nonsense rather than fail.
enum ProbeField : int32_t {
  kSimd = 0,
  kThreads = 1,
  kSharedMemory = 2,
  kHardwareConcurrency = 3,
  kProbeFieldCount = 4,
};

}  // namespace

extern "C" {

// Number of int32 slots the caller must allocate for sph_probe_runtime's output.
EMSCRIPTEN_KEEPALIVE int32_t sph_probe_field_count() { return kProbeFieldCount; }

// Writes kProbeFieldCount int32 values into `out`. Returns 0 on success, non-zero if the caller
// passed a null buffer — no exceptions cross this boundary (ADR 0006).
EMSCRIPTEN_KEEPALIVE int32_t sph_probe_runtime(int32_t concurrency, int32_t crossOriginIsolated,
                                               int32_t* out) {
  if (out == nullptr) return 1;

  const auto caps = sphanorama::bridge::ProbeRuntime(concurrency, crossOriginIsolated != 0);
  out[kSimd] = caps.simd ? 1 : 0;
  out[kThreads] = caps.threads ? 1 : 0;
  out[kSharedMemory] = caps.sharedMemory ? 1 : 0;
  out[kHardwareConcurrency] = caps.hardwareConcurrency;
  return 0;
}

}  // extern "C"
