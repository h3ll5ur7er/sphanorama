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
