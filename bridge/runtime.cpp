#include "runtime.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/heap.h>

#include <algorithm>
#endif

namespace sphanorama::bridge {

#if defined(__EMSCRIPTEN__)
int64_t Runtime::BrowserHeapCeilingBytes() {
  // Stated, not probed, and the number is chosen to be survivable rather than comfortable: a
  // mid-range phone that lets a tab reach a few hundred megabytes is not guaranteed, and the
  // failure when it does not is the tab dying rather than an allocation returning null. 128 MB
  // holds a few dozen full-resolution frames, which is a burst or two — everything past that is
  // what the spill tier is for.
  constexpr int64_t kStated = 128ll << 20;

  // Clamped by what the module was linked to allow. That is a real limit rather than a guess,
  // even though it is the build's rather than the device's: a ceiling above it could never be
  // reached, so a store that believed it would refuse to spill until the heap had already failed
  // to grow. Three quarters of it, because the heap holds more than frames.
  const int64_t linked = static_cast<int64_t>(emscripten_get_heap_max()) / 4 * 3;
  return std::min(kStated, linked);
}
#endif


Runtime& Runtime::Instance() {
  static Runtime runtime;
  return runtime;
}

}  // namespace sphanorama::bridge
