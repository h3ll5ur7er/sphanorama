#include "utilities/quaternion.h"

#include <algorithm>
#include <cmath>

namespace sphanorama {

double Norm(const Quat& q) {
  return std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
}

Quat Normalize(const Quat& q) {
  const double norm = Norm(q);
  if (!(norm > 1e-12)) return Quat{};   // also catches NaN, which compares false against >
  return Quat{q.w / norm, q.x / norm, q.y / norm, q.z / norm};
}

Quat FromAxisAngle(const Vec3& axis, double radians) {
  const double length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
  if (!(length > 1e-12)) return Quat{};

  const double half = radians * 0.5;
  const double s = std::sin(half) / length;
  return Quat{std::cos(half), axis.x * s, axis.y * s, axis.z * s};
}

double AngleBetween(const Quat& a, const Quat& b) {
  const Quat p = Normalize(a);
  const Quat q = Normalize(b);

  // |dot| rather than dot: q and -q are the same rotation, and using the signed value would
  // report a sign flip in the sensor stream as a 180-degree jump.
  double dot = std::abs(p.w * q.w + p.x * q.x + p.y * q.y + p.z * q.z);
  dot = std::clamp(dot, 0.0, 1.0);   // guard acos against rounding past 1
  return 2.0 * std::acos(dot);
}

Quat Multiply(const Quat& a, const Quat& b) {
  return Quat{
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
  };
}

Quat Conjugate(const Quat& q) {
  const Quat n = Normalize(q);
  return Quat{n.w, -n.x, -n.y, -n.z};
}

Vec3 Rotate(const Quat& q, const Vec3& v) {
  // v + 2w(u x v) + 2(u x (u x v)), with u the vector part — the standard form, which avoids
  // building a matrix for a single vector.
  const Quat n = Normalize(q);
  const Vec3 u{n.x, n.y, n.z};
  const Vec3 uv{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
  const Vec3 uuv{u.y * uv.z - u.z * uv.y, u.z * uv.x - u.x * uv.z, u.x * uv.y - u.y * uv.x};
  return Vec3{
      v.x + 2.0 * (n.w * uv.x + uuv.x),
      v.y + 2.0 * (n.w * uv.y + uuv.y),
      v.z + 2.0 * (n.w * uv.z + uuv.z),
  };
}

Quat FromAzimuthElevation(double azimuthDeg, double elevationDeg) {
  constexpr double kDegToRad = 0.017453292519943295;
  // Yaw first, then pitch in the yawed frame: azimuth sweeps the horizon and elevation lifts out
  // of it, which is how a capture plan is read and how a user turns.
  const Quat yaw = FromAxisAngle(Vec3{0, 1, 0}, azimuthDeg * kDegToRad);
  const Quat pitch = FromAxisAngle(Vec3{1, 0, 0}, elevationDeg * kDegToRad);
  return Normalize(Multiply(yaw, pitch));
}

Vec3 Direction(const Quat& q) {
  const Quat n = Normalize(q);
  // -Z rotated by n, expanded rather than going through a matrix.
  return Vec3{
      -2.0 * (n.x * n.z + n.w * n.y),
      -2.0 * (n.y * n.z - n.w * n.x),
      -(1.0 - 2.0 * (n.x * n.x + n.y * n.y)),
  };
}

}  // namespace sphanorama
