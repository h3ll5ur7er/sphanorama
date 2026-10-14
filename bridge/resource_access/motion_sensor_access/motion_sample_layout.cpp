#include "resource_access/motion_sensor_access/motion_sample_layout.h"

#include <algorithm>
#include <cstdint>

#include "sphanorama/wire.h"

namespace sphanorama::bridge {

size_t DecodeImuSamples(std::span<const double> flat, std::span<ImuSample> out) {
  const size_t taken = std::min(flat.size() / kDoublesPerSample, out.size());
  // Two indices, because the host's read is destructive. `motionDrain` splices what it returns out
  // of its own buffer, so by the time this loop runs the page has already let go of all `taken`
  // samples: whatever is skipped here is not left behind for the next drain, it is gone. An early
  // return therefore threw away up to sixty-three good samples to refuse one bad one, and reported
  // a count the host's own position no longer agreed with. `written` compacts instead.
  size_t written = 0;
  for (size_t i = 0; i < taken; ++i) {
    const double* f = flat.data() + i * kDoublesPerSample;
    ImuSample& sample = out[written];
    // Checked before the cast, not after — by the time a non-finite or out-of-range double has
    // been converted to `int64_t` the behaviour is already undefined, which is the same sentence
    // `runtime.cpp` and `browser_project_store_access.cpp` carry over the same kind of line. This
    // was the third door in `bridge/` and the only one that did not ask.
    //
    // Dropped rather than clamped or zeroed: a timestamp is what `OrientationPoseEngine::Integrate`
    // measures its window with, so a made-up one is a made-up rotation. `Drain` reports how many
    // samples it wrote, so returning fewer is an answer the contract already has a shape for,
    // where an invented sample is not.
    if (!wire::IsRepresentableInt64(f[0])) continue;
    sample.timestampNs = static_cast<int64_t>(f[0]);
    sample.hasAngularVelocity = f[1] != 0.0;
    sample.angularVelocity = Vec3{f[2], f[3], f[4]};
    sample.acceleration = Vec3{f[5], f[6], f[7]};
    sample.hasMagnetometer = f[8] != 0.0;
    sample.magneticField = Vec3{f[9], f[10], f[11]};
    sample.hasOrientation = f[12] != 0.0;
    sample.orientation = Quat{f[13], f[14], f[15], f[16]};
    sample.screenAngleDeg = f[17];
    ++written;
  }
  // The attitude and the rate are *not* checked here, and that is deliberate rather than an
  // omission left over from the timestamp. `OrientationPoseEngine::Integrate` asks whether each is
  // a measurement before it believes the flag beside it, which is the layer that decides what
  // "measured" means and covers every port rather than this one. The timestamp has no such reader:
  // it is compared and subtracted directly, and converting it is undefined before anyone sees it.
  return written;
}

}  // namespace sphanorama::bridge
