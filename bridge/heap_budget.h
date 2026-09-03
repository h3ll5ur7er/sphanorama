#pragma once

#include <cstdint>

namespace sphanorama::bridge {

// How much of the WASM heap the frame store may spend, decided from what the device reports.
//
// `FrameStoreBudget::heapCeilingBytes` is documented as "measured at startup, not assumed", and
// for a long time both platforms named a constant instead. This is the measurement — such as it
// can be, which is a distinction worth stating rather than glossing.
//
// **What is measured and what is not.** `navigator.deviceMemory` is the device's RAM, so a
// ceiling derived from it scales with the machine rather than standing still: a 1 GB phone and an
// 8 GB desktop no longer get the same number. `emscripten_get_heap_max()` is the build's own
// limit, which is a fact rather than an estimate. Neither answers the question one would actually
// like answered — *how much will this tab be allowed to keep* — and nothing in the platform does.
//
// **Why there is no allocation probe.** The obvious next step is to try the number and see. Two
// things rule it out. WASM heap growth is one-way, so an allocation that tested 512 MB would
// leave a 512 MB heap behind whether or not the session ever needed it. And pushing until the
// allocator refuses is precisely the approach to the cliff this ceiling exists to keep the
// session away from: on a phone the browser may kill the tab before malloc ever returns null.
// A probe that risks the failure it is measuring is not a measurement.
//
// So: read the device, take a modest share, clamp by what the build allows, and keep a floor that
// leaves one burst possible. Pure, so the arithmetic is tested natively; the browser part is the
// two numbers it is handed.

// What to use when the browser will not report device memory — Safari and Firefox do not, so this
// is every iPhone rather than an edge case. It is the number this code used before it could ask,
// which is the honest choice: no new information, no new answer.
inline constexpr int64_t kStatedCeilingBytes = 128ll << 20;

// Below this a store cannot hold one default burst of downscaled frames, and a store that cannot
// hold a burst cannot capture a cell at all. Degraded capture beats no capture, so the share of
// device memory is allowed to round up to here — but never past what the module can address.
inline constexpr int64_t kFloorCeilingBytes = 48ll << 20;

// The share of the device's RAM a single tab's frame store may claim. One sixteenth puts a 4 GB
// phone at 256 MB and a 1 GB phone at 64 MB. The browser, the rest of the page and every other
// tab live in the same memory, and the one that gets killed for asking too much is this one.
inline constexpr int64_t kDeviceMemoryShare = 16;

// Of the linked heap, how much may be frames. The heap also holds the plan, the candidate sets,
// the allocator's bookkeeping and every temporary an engine makes; a ceiling equal to the linked
// maximum would have the store consider itself within budget at the moment malloc began failing
// for everything else.
inline constexpr int64_t kLinkedHeapNumerator = 3;
inline constexpr int64_t kLinkedHeapDenominator = 4;

// `deviceMemoryBytes` is navigator.deviceMemory converted to bytes, or 0 when the browser will
// not say. `linkedMaxBytes` is emscripten_get_heap_max(). Both are treated as untrusted: they
// cross from JavaScript, and a ceiling of zero would make the store refuse every allocation,
// which reads to a user as a broken camera rather than a full one.
int64_t ChooseHeapCeiling(int64_t deviceMemoryBytes, int64_t linkedMaxBytes);

}  // namespace sphanorama::bridge
