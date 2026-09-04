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
