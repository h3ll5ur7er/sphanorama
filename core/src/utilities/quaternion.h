#pragma once

#include "sphanorama/types.h"

namespace sphanorama {

// Orientation maths shared by the coverage planner, the pose engine and registration.
//
// Every function here is total: degenerate input (a zero quaternion from an uninitialised sensor
// read, a zero axis) yields identity rather than NaN. NaN does not fail loudly — it propagates
// through a whole capture session and surfaces as a sphere that will not close.

double Norm(const Quat& q);

// Returns identity for a degenerate quaternion rather than dividing by zero.
Quat Normalize(const Quat& q);

Quat FromAxisAngle(const Vec3& axis, double radians);

// Rotation separating two orientations, in radians, in [0, pi]. Treats q and -q as the same
// orientation, because they are.
double AngleBetween(const Quat& a, const Quat& b);

// The direction the device is looking: -Z rotated by the orientation, matching the convention
// the browser's DeviceOrientation reports against.
Vec3 Direction(const Quat& q);

}  // namespace sphanorama
