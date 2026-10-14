#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "sphanorama/types.h"

namespace sphanorama::bridge {

// The layout capture-host.ts writes. Read in another layout, the doubles decode into plausible
// values rather than failing, which is why the order is spelled out in both places rather than
// implied by a struct — and why each side names its layout to the other before anything is read:
// the page and the core are fetched and cached separately, so the first launch after a deploy can
// pair one with the other's predecessor.
//
// Apart from the port because the port needs Emscripten, and this is the half a native test can
// hold to the order.
inline constexpr size_t kDoublesPerSample = 18;

// Which layout that is: MOTION_SAMPLE_LAYOUT in capture-host.ts. Bumped on any change to the order,
// units or sense of a field, not only the length, since a reordering that keeps the count decodes
// as quietly wrong as one that does not. Every page and core before 2 names none.
inline constexpr int32_t kMotionSampleLayout = 2;

// Decodes whole samples from `flat` into `out`, as many as both hold, and returns how many it
// wrote. A sample whose timestamp is not a representable integer is dropped and the rest are
// compacted over it.
size_t DecodeImuSamples(std::span<const double> flat, std::span<ImuSample> out);

}  // namespace sphanorama::bridge
