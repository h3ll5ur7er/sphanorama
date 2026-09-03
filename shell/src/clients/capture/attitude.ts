/**
 * The sensor readout, in the frame the user is reasoning about.
 *
 * This is a diagnostic line, not guidance: aim and acceptance come back from the core and are
 * rendered by `describeGuidance`. It exists because the two sources behind `IMotionSensorAccess`
 * report different things — an Euler triple and a quaternion — and neither is readable next to a
 * phone in a hand. Azimuth, elevation and roll are, and they are the same three numbers the
 * coverage plan is written in.
 */
import type { Quat } from '../../../../contracts/ts/contracts';

const RAD_TO_DEG = 180 / Math.PI;

type Vec3 = { x: number; y: number; z: number };

/** Where the camera looks: -Z rotated by the attitude. */
function direction(q: Quat): Vec3 {
  const { w, x, y, z } = q;
  return {
    x: -2 * (x * z + w * y),
    y: -2 * (y * z - w * x),
    z: -(1 - 2 * (x * x + y * y)),
  };
}

/** The right edge of the picture: +X rotated by the attitude. */
function rightEdge(q: Quat): Vec3 {
  const { w, x, y, z } = q;
  return { x: 1 - 2 * (y * y + z * z), y: 2 * (x * y + w * z), z: 2 * (x * z - w * y) };
}

const dot = (a: Vec3, b: Vec3) => a.x * b.x + a.y * b.y + a.z * b.z;

const cross = (a: Vec3, b: Vec3): Vec3 => ({
  x: a.y * b.z - a.z * b.y,
  y: a.z * b.x - a.x * b.z,
  z: a.x * b.y - a.y * b.x,
});

/** -0 prints as "-0", which reads as a sign error in a column of angles that are otherwise zero. */
const whole = (deg: number) => (Math.round(deg) === 0 ? 0 : Math.round(deg));

export function describeAttitude(orientation: Quat): string {
  const aim = direction(orientation);

  // The inverse of FromAzimuthElevation: yaw about +Y, then pitch about +X, so the looking
  // direction is (-cos el · sin az, sin el, -cos el · cos az).
  const elevation = Math.asin(Math.max(-1, Math.min(1, aim.y))) * RAD_TO_DEG;
  const azimuth = Math.atan2(-aim.x, -aim.z) * RAD_TO_DEG;

  // Roll against the level cell that shares this direction, measured exactly as RollBetween
  // measures it: from the target's horizontal +X axis to the camera's, about the viewing axis.
  // At a pole the azimuth is arbitrary and so is the reference, but it is still perpendicular to
  // the aim, so the answer is a number rather than a NaN.
  const level: Vec3 = {
    x: Math.cos(azimuth / RAD_TO_DEG), y: 0, z: -Math.sin(azimuth / RAD_TO_DEG),
  };
  const here = rightEdge(orientation);
  const roll = Math.atan2(dot(cross(level, here), aim), dot(level, here)) * RAD_TO_DEG;

  return `az ${whole((azimuth + 360) % 360)}° el ${whole(elevation)}° roll ${whole(roll)}°`;
}
