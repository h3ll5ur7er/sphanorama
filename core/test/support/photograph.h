#pragma once

namespace sphanorama::test {

// The photographed panorama the accuracy table is measured in (ADR 0059), relative to the
// repository root, which is where the generator's command runs. Its own header so that the
// WebAssembly runner, which cannot include the native renderer, reports the same path.
inline constexpr const char* kPhotograph = "core/test/data/panoramas/small_hangar_01_1k.jpg";

}  // namespace sphanorama::test
