#include "runtime.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/em_js.h>
#include <emscripten/heap.h>

#include "heap_budget.h"
#endif

namespace sphanorama::bridge {

#if defined(__EMSCRIPTEN__)
namespace {

// The device's RAM in bytes, or 0 when the browser will not say.
//
// `navigator.deviceMemory` is quantised to a handful of values and capped at 8 GiB, which is
// coarse and is fine: what the ceiling needs is the difference between a 1 GB phone and an 8 GB
// desktop, not a byte count. It is available on WorkerNavigator, which matters because the core
// runs in a worker (ADR 0019) and there is no `window` here to ask.
//
// Chromium reports it; Safari and Firefox do not, on the grounds that it fingerprints the device.
// That is a real cost and this reads it anyway for one number that never leaves the tab — but it
// does mean every iPhone takes the fallback, which is the situation the fallback is sized for.
EM_JS(double, host_device_memory_gib, (), {
  const reported = navigator.deviceMemory;
  return typeof reported === 'number' && reported > 0 ? reported : 0;
});

}  // namespace

int64_t Runtime::BrowserHeapCeilingBytes() {
  const double gib = host_device_memory_gib();
  const int64_t deviceMemory = static_cast<int64_t>(gib * static_cast<double>(1ll << 30));
  return ChooseHeapCeiling(deviceMemory, static_cast<int64_t>(emscripten_get_heap_max()));
}
#endif


Runtime& Runtime::Instance() {
  static Runtime runtime;
  return runtime;
}

}  // namespace sphanorama::bridge
