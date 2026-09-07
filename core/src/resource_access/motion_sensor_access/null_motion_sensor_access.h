#pragma once

#include "sphanorama/resource_access/motion_sensor_access.h"

namespace sphanorama {

// Stands in wherever no real port is installed — the native and bench runtimes, and any build
// before the browser port loads. Reports MotionCapability::None.
//
// That answer used to be a supported configuration; since ADR 0044 it is what refuses a capture
// session, so a runtime wired to this port cannot open one. That is the honest outcome rather
// than an inconvenience: nothing here can say which way a camera is pointing, and a sphere whose
// cells are labelled with directions nobody measured is worse than the refusal. A bench that
// wants to drive a session needs a port that replays orientation — which is ADR 0044's own
// consequence, and is where that sentence lives. (It cited `docs/03` UC-4, which says nothing of
// the kind; a reviewer read the citation rather than trusting it.)
class NullMotionSensorAccess final : public IMotionSensorAccess {
 public:
  Result<MotionCapability> Capabilities() override;
  Status Start(int32_t requestedHz) override;
  Result<int32_t> Drain(std::span<ImuSample> out) override;
  Status Stop() override;
};

}  // namespace sphanorama
