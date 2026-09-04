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

  // A linked maximum of zero means the platform did not answer either — natively, or a browser
  // build that reports nothing. Falling back to `wanted` is right: there is no fact to defer to,
  // and refusing to allocate at all is the one outcome that is certainly wrong.
  //
  // The silence is read off `linkedMaxBytes` itself rather than off the share of it, because the
  // share of a very small number is zero: a heap of three bytes would otherwise be mistaken for a
  // platform that said nothing, and the clamp it asked for would be dropped. A number is a number
  // however small, and only the absence of one is silence.
  //
  // The share is divided before it is multiplied. That truncates twice and so lands up to two
  // bytes below three quarters exactly — immaterial in a ceiling measured in megabytes, and the
  // price of an expression that cannot overflow for any int64_t this is handed. Multiplying first
  // would be exact and would be undefined behaviour near the top of the range, which is not a
  // trade to make with an untrusted input.
  const int64_t ceiling =
      linkedMaxBytes > 0
          ? std::min(wanted, linkedMaxBytes / kLinkedHeapDenominator * kLinkedHeapNumerator)
          : wanted;

  // Never zero, however nonsensical the inputs were. Both numbers cross from JavaScript through a
  // boundary that has been wrong before, and a store with a ceiling of zero refuses every
  // allocation — which a user reads as a broken camera rather than a full one.
  return std::max<int64_t>(ceiling, 1);
}

}  // namespace sphanorama::bridge
