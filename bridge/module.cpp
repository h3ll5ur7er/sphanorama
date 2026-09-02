#include "module.h"

#include "capabilities.h"

#include "export.h"

namespace {

// Field order is the ABI. It is published through sph_probe_field_name rather than duplicated in
// JavaScript, so adding or reordering a field cannot desync the two sides.
enum ProbeField : int32_t {
  kSimd = 0,
  kThreads,
  kSharedMemory,
  kHardwareConcurrency,
  kProbeFieldCount,
};

constexpr const char* kProbeFieldNames[] = {
    "simd",
    "threads",
    "sharedMemory",
    "hardwareConcurrency",
};

static_assert(sizeof(kProbeFieldNames) / sizeof(kProbeFieldNames[0]) == kProbeFieldCount,
              "every probe field needs a published name, or the boundary lies about its layout");

}  // namespace

extern "C" {

SPH_EXPORT int32_t sph_probe_field_count() { return kProbeFieldCount; }

SPH_EXPORT const char* sph_probe_field_name(int32_t index) {
  if (index < 0 || index >= kProbeFieldCount) return nullptr;
  return kProbeFieldNames[index];
}

SPH_EXPORT int32_t sph_probe_runtime(int32_t concurrency, int32_t crossOriginIsolated,
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
