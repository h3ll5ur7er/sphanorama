#include "runtime.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/em_js.h>
#include <emscripten/heap.h>

#include <cmath>

#include "heap_budget.h"
#endif

namespace sphanorama::bridge {

#if defined(__EMSCRIPTEN__)
namespace {

// The device's RAM in **gibibytes**, as `navigator.deviceMemory` reports it, or 0 when there is
// no answer. The caller converts; the units are named here because the two are one multiplication
// apart and a comment that got them wrong is how a ceiling ends up a billion times too large.
//
// `deviceMemory` is quantised to a handful of values and capped at 8, which is coarse and is
// fine: what the ceiling needs is the difference between a 1 GB phone and an 8 GB desktop, not a
// byte count. It lives on WorkerNavigator too, which matters because the core runs in a worker
// (ADR 0019) and there is no `window` here to ask.
//
// Chromium reports it; Safari and Firefox do not, on the grounds that it fingerprints the device.
// That is a real cost and this reads it anyway for one number that never leaves the tab — but it
// does mean every iPhone takes the fallback, which is the situation the fallback is sized for.
//
// `navigator` itself is guarded rather than assumed. An EM_JS body that throws aborts the module,
// so a missing global here would not be a missing ceiling — it would be a core that never boots,
// which is a spectacular failure for an optional hint.
EM_JS(double, host_device_memory_gib, (), {
  if (typeof navigator === 'undefined') return 0;
  const reported = navigator.deviceMemory;
  return typeof reported === 'number' && Number.isFinite(reported) && reported > 0 ? reported : 0;
});

}  // namespace

int64_t Runtime::BrowserHeapCeilingBytes() {
  // Checked before the cast, not after. `ChooseHeapCeiling` treats its inputs as untrusted, but it
  // cannot: by the time a non-finite or wildly out-of-range double has been converted to int64_t
  // the behaviour is already undefined, and what arrives is whatever the conversion happened to
  // produce. The guard has to be on this side of it.
  //
  // The bound is far above anything the spec allows — `deviceMemory` is capped at 8 — so it is a
  // sanity check on the boundary rather than a policy about machines.
  constexpr double kImplausibleGiB = 1024.0;
  const double gib = host_device_memory_gib();
  const bool usable = std::isfinite(gib) && gib > 0.0 && gib < kImplausibleGiB;
  const int64_t deviceMemory =
      usable ? static_cast<int64_t>(gib * static_cast<double>(1ll << 30)) : 0;
  return ChooseHeapCeiling(deviceMemory, static_cast<int64_t>(emscripten_get_heap_max()));
}
#endif


Runtime& Runtime::Instance() {
  static Runtime runtime;
  return runtime;
}

}  // namespace sphanorama::bridge
