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

Quat Multiply(const Quat& a, const Quat& b);

// The inverse rotation, for a unit quaternion.
Quat Conjugate(const Quat& q);

// A vector rotated by an orientation.
Vec3 Rotate(const Quat& q, const Vec3& v);

// Orientation looking at a point on the sphere. Azimuth turns about +Y from the forward axis;
// elevation lifts toward +Y. This is the convention the coverage plan is expressed in.
Quat FromAzimuthElevation(double azimuthDeg, double elevationDeg);

// The direction the device is looking: -Z rotated by the orientation, matching the convention
// the browser's DeviceOrientation reports against.
Vec3 Direction(const Quat& q);

double Dot(const Vec3& a, const Vec3& b);
Vec3 Cross(const Vec3& a, const Vec3& b);

// Returns the zero vector for a degenerate input rather than dividing by zero, so callers can
// test for it instead of propagating NaN through a whole session.
Vec3 Normalize(const Vec3& v);

// The angle between two directions, in radians, in [0, pi]. Unlike AngleBetween on orientations
// this ignores rotation about the axis — which is what "how far off am I aiming" means.
double AngleBetweenDirections(const Vec3& a, const Vec3& b);

// Rotation about the viewing axis separating two orientations, in radians, signed and in
// (-pi, pi]. Zero when both are held the same way up. Zero, too, when the two look in opposite
// directions, where roll has no meaning.
double RollBetween(const Quat& current, const Quat& target);

}  // namespace sphanorama
