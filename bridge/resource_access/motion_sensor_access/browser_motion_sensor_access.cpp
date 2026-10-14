#include "resource_access/motion_sensor_access/browser_motion_sensor_access.h"

#include <emscripten/emscripten.h>

#include <algorithm>
#include <vector>

#include "resource_access/motion_sensor_access/motion_sample_layout.h"

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

EM_JS(int32_t, host_motion_drain, (double* out, int32_t maxSamples, int32_t stride), {
  const host = Module.sphHost;
  if (!host || !host.motionDrain) return 0;
  // A page that does not write this layout gets nothing read from it, and a page from before the
  // layout said which it was does not say at all.
  if (host.motionSampleDoubles !== stride) return 0;
  const flat = host.motionDrain(maxSamples, stride);
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
  // them straight into the caller's span. Eighteen EM_JS calls per sample at 60Hz would cost more
  // in boundary crossings than the samples are worth.
  std::vector<double> flat(out.size() * kDoublesPerSample);
  const int32_t count = host_motion_drain(flat.data(), static_cast<int32_t>(out.size()),
                                          static_cast<int32_t>(kDoublesPerSample));
  if (count <= 0) return Ok(0);

  // Destructive on the page's side: `motionDrain` has already let go of every sample it returned,
  // so what the decoder drops is gone rather than left for the next drain.
  const auto taken = std::min(static_cast<size_t>(count), out.size());
  return Ok(static_cast<int32_t>(
      DecodeImuSamples(std::span<const double>(flat.data(), taken * kDoublesPerSample), out)));
}

Status BrowserMotionSensorAccess::Stop() {
  running_ = false;
  // The page's buffer is dropped too: samples from before a stop describe a pose nobody asked
  // for, and handing them to the next session would start it looking the wrong way.
  host_motion_reset();
  return Status::Ok();
}

}  // namespace sphanorama::bridge
