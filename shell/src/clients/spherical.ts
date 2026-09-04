/**
 * The two angles a sphere is discussed in, shared by the clients that discuss it.
 *
 * Both clients need to turn an attitude into a place: the capture client to say where the phone
 * is pointing, the review client to put a cell on a map. It is one conversion and it has to be
 * the same one — the core plans in azimuth and elevation, and a client that derived them its own
 * way would put the reticle and the map in frames that disagree by a sign nobody would notice
 * until a capture came out mirrored.
 *
 * Presentation rather than business logic, which is why it lives with the clients: the core's own
 * `FromAzimuthElevation` is the definition, and this is its inverse for display.
 */
import type { Quat } from '../../../contracts/ts/contracts';

const RAD_TO_DEG = 180 / Math.PI;

export type Vec3 = { x: number; y: number; z: number };

/** Where the camera looks: -Z rotated by the attitude. */
export function direction(q: Quat): Vec3 {
  const { w, x, y, z } = q;
  return {
    x: -2 * (x * z + w * y),
    y: -2 * (y * z - w * x),
    z: -(1 - 2 * (x * x + y * y)),
  };
}

/**
 * A world direction expressed in the frame of something with this attitude.
 *
 * The inverse of what `direction` does: that takes the camera's own forward axis out into the
 * world, and this brings a world direction back in. Used to ask where a cell falls in the picture,
 * which is a question about the camera's frame — and answering it by comparing world directions
 * instead would lose the roll, so a tilted phone would draw its markers upright.
 */
export function rotateInto(q: Quat, world: Vec3): Vec3 {
  // Rotating by the conjugate, written out rather than composed from a multiply and an inverse:
  // the vector part is negated once, here, where it is visible.
  const [x, y, z, w] = [-q.x, -q.y, -q.z, q.w];
  const tx = 2 * (y * world.z - z * world.y);
  const ty = 2 * (z * world.x - x * world.z);
  const tz = 2 * (x * world.y - y * world.x);
  return {
    x: world.x + w * tx + (y * tz - z * ty),
    y: world.y + w * ty + (z * tx - x * tz),
    z: world.z + w * tz + (x * ty - y * tx),
  };
}

export interface Aim {
  /** Degrees clockwise from straight ahead, in (-180, 180]. */
  azimuthDeg: number;
  /** Degrees above the horizon, in [-90, 90]. */
  elevationDeg: number;
}

/**
 * The inverse of the core's `FromAzimuthElevation`: yaw about +Y, then pitch about +X, so the
 * looking direction is (-cos el · sin az, sin el, -cos el · cos az).
 *
 * At a pole the azimuth is arbitrary rather than wrong — every azimuth points there — and
 * `atan2(0, 0)` answers 0, which is as good as any and is at least a number.
 */
export function aimOf(orientation: Quat): Aim {
  return aimOfDirection(direction(orientation));
}

/**
 * The same conversion for a caller that already has the looking direction in hand.
 *
 * Split out because a caller needing both the direction and the angles would otherwise derive the
 * direction twice — not a cost worth counting, but two derivations of one value in one function
 * is the kind of thing that drifts when somebody edits one of them.
 */
export function aimOfDirection(aim: Vec3): Aim {
  return {
    elevationDeg: Math.asin(Math.max(-1, Math.min(1, aim.y))) * RAD_TO_DEG,
    azimuthDeg: Math.atan2(-aim.x, -aim.z) * RAD_TO_DEG,
  };
}
