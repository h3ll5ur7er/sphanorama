#include "resource_access/motion_sensor_access/browser_motion_sensor_access.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <vector>

#include "sphanorama/wire.h"

namespace sphanorama::bridge {
namespace {

constexpr const char* kComponent = "BrowserMotionSensorAccess";

// Returned as an index into MotionCapability rather than a string: the enum's order is the
// contract, and the generated codec encodes it the same way.
EM_JS(int32_t, host_motion_capability, (), {
  if (!Module.sphHost) return 0;
  const order = ['None', 'OrientationOnly', 'GyroAccel', 'GyroAccelMag'];
  const index = order.indexOf(Module.sphHost.motionCapability());
  return index < 0 ? 0 : index;
});

// The layout capture-host.ts writes. Both sides are pinned by a test; changing one without the
// other decodes into plausible nonsense rather than failing, which is why the order is spelled
// out in both places rather than being implied by a struct.
constexpr size_t kDoublesPerSample = 17;

EM_JS(int32_t, host_motion_drain, (double* out, int32_t maxSamples, int32_t stride), {
  const host = Module.sphHost;
  if (!host || !host.motionDrain) return 0;
  const flat = host.motionDrain(maxSamples);
  // Bounded here, before the write, rather than trusted from the host. `host_preview_copy` three
  // functions away takes exactly this view of exactly this question — "the two are read at
  // different moments, and a frame swapped in between would write past the end of the span" — and
  // this one took the host's word for it, which a reviewer pointed out is the same door with the
  // opposite answer. The page's own splice already bounds it; that is a reason for the two to
  // agree, not for one of them to skip the check.
  const room = maxSamples * stride;
  if (flat.length > room) return 0;
  // HEAPF64 is read fresh: Emscripten replaces the view when memory grows, and a cached one
  // would be detached.
  Module.HEAPF64.set(flat, out >> 3);
  // The stride is passed in rather than written here, because it used to be a third copy of the
  // layout's length and the one nothing would have caught: a decoder reading the wrong number of
  // samples produces plausible ones.
  return flat.length / stride;
});

EM_JS(void, host_motion_reset, (), {
  const host = Module.sphHost;
  if (host && host.resetMotion) host.resetMotion();
});

}  // namespace

Result<MotionCapability> BrowserMotionSensorAccess::Capabilities() {
  return Ok(static_cast<MotionCapability>(host_motion_capability()));
}

Status BrowserMotionSensorAccess::Start(int32_t requestedHz) {
  if (requestedHz <= 0) {
    return Fail(StatusCode::InvalidArgument, kComponent, "sample rate must be positive");
  }
  // The page started the sensor before it began a session; there is nothing to start here except
  // to record that a session is now entitled to drain.
  if (host_motion_capability() == 0) {
    return Fail(StatusCode::SensorUnavailable, kComponent, "the page reports no motion sensors");
  }
  running_ = true;
  return Status::Ok();
}

Result<int32_t> BrowserMotionSensorAccess::Drain(std::span<ImuSample> out) {
  if (!running_) {
    return Err<int32_t>(StatusCode::FailedPrecondition, kComponent, "sensor is not running");
  }
  if (out.empty()) return Ok(0);

  // One call per drain, not one per field: the host returns a flat run of doubles and this writes
  // them straight into the caller's span. Sixteen EM_JS calls per sample at 60Hz would cost more
  // in boundary crossings than the samples are worth.
  std::vector<double> flat(out.size() * kDoublesPerSample);
  const int32_t count = host_motion_drain(flat.data(), static_cast<int32_t>(out.size()),
                                          static_cast<int32_t>(kDoublesPerSample));
  if (count <= 0) return Ok(0);

  const auto taken = std::min(static_cast<size_t>(count), out.size());
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
    ++written;
  }
  // The attitude and the rate are *not* checked here, and that is deliberate rather than an
  // omission left over from the timestamp. `OrientationPoseEngine::Integrate` asks whether each is
  // a measurement before it believes the flag beside it, which is the layer that decides what
  // "measured" means and covers every port rather than this one. The timestamp has no such reader:
  // it is compared and subtracted directly, and converting it is undefined before anyone sees it.
  return Ok(static_cast<int32_t>(written));
}

Status BrowserMotionSensorAccess::Stop() {
  running_ = false;
  // The page's buffer is dropped too: samples from before a stop describe a pose nobody asked
  // for, and handing them to the next session would start it looking the wrong way.
  host_motion_reset();
  return Status::Ok();
}

}  // namespace sphanorama::bridge
