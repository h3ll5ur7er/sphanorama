#include "heap_budget.h"

#include <algorithm>

namespace sphanorama::bridge {

int64_t ChooseHeapCeiling(int64_t deviceMemoryBytes, int64_t linkedMaxBytes) {
  // What the device suggests, or the stated fallback when it will not say. Negative is nonsense
  // rather than a small device, and is treated as "did not say" for the same reason.
  const int64_t fromDevice = deviceMemoryBytes > 0 ? deviceMemoryBytes / kDeviceMemoryShare
                                                   : kStatedCeilingBytes;

  // Rounded up to the floor, so a device that reports almost nothing still gets a store that can
  // hold a burst. This happens before the linked clamp deliberately: a floor is a policy about
  // what makes capture possible, and the linked maximum is a fact about what the module can
  // address. Where they disagree the fact has to win, or the store believes in memory that does
  // not exist.
  const int64_t wanted = std::max(fromDevice, kFloorCeilingBytes);

  const int64_t linked =
      linkedMaxBytes > 0 ? linkedMaxBytes / kLinkedHeapDenominator * kLinkedHeapNumerator : 0;
  // A linked maximum of zero means the platform did not answer either — natively, or a browser
  // build that reports nothing. Falling back to `wanted` is right: there is no fact to defer to,
  // and refusing to allocate at all is the one outcome that is certainly wrong.
  const int64_t ceiling = linked > 0 ? std::min(wanted, linked) : wanted;

  // Never zero, however nonsensical the inputs were. Both numbers cross from JavaScript through a
  // boundary that has been wrong before, and a store with a ceiling of zero refuses every
  // allocation — which a user reads as a broken camera rather than a full one.
  return std::max<int64_t>(ceiling, 1);
}

}  // namespace sphanorama::bridge
