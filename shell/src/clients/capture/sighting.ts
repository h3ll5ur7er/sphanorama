/**
 * Where a planned cell falls in the viewfinder, and which way to turn when it does not.
 *
 * This is the geometry behind the capture overlay: a ring drawn on each cell the user can see,
 * and an arrow toward the one they cannot. It is presentation — *which* cell to go to is the
 * planner's answer, arriving as `CaptureGuidance.targetNode`, and nothing here re-decides it.
 *
 * The field of view comes from the plan the core built rather than being derived a second time,
 * so the frame the markers are drawn in and the frame the cells were spaced in are the same one
 * by construction. A second derivation is how a marker ends up next to the cell it names.
 */
import type { Quat } from '../../../../contracts/ts/contracts';
import { direction, rotateInto } from '../spherical';

const DEG_TO_RAD = Math.PI / 180;
const RAD_TO_DEG = 180 / Math.PI;

/** The two angles the camera sees, as the plan records them. */
export interface Lens {
  horizontalFovDeg: number;
  verticalFovDeg: number;
}

/**
 * Where something is, from the viewfinder's point of view.
 *
 * A union rather than coordinates plus a flag, because off screen there is no honest x and y to
 * report — behind you the perspective divide changes sign and would place a cell on the opposite
 * side of the frame, pointing the user away from it. Making the coordinates unreachable in that
 * case means a renderer cannot use them by accident.
 */
export type Sighting =
  | { readonly onScreen: true; readonly x: number; readonly y: number; readonly bearingDeg: number }
  | { readonly onScreen: false; readonly bearingDeg: number };

/**
 * Degrees clockwise from straight up: 90 is to the right, 270 to the left.
 *
 * Taken from the target's offset across the picture, which stays meaningful behind the camera —
 * a cell behind you and to the left is still reached by turning left. Directly ahead or directly
 * behind it answers zero, which is arbitrary and has to be: every direction is equally correct,
 * and a number beats a NaN reaching the DOM.
 */
function bearingOf(x: number, y: number): number {
  const clockwiseFromUp = Math.atan2(x, y) * RAD_TO_DEG;
  return (clockwiseFromUp + 360) % 360;
}

export function sight(attitude: Quat, target: Quat, lens: Lens): Sighting {
  // The cell's direction as the camera sees it: +X right, +Y up, -Z straight ahead.
  const seen = rotateInto(attitude, direction(target));
  const bearingDeg = bearingOf(seen.x, seen.y);

  // How far ahead it is. Zero or less is beside or behind the camera, where a perspective divide
  // has no answer — not a small one, a wrong one, because the sign flips the picture.
  const ahead = -seen.z;
  if (ahead <= 0) return { onScreen: false, bearingDeg };

  // Half the frame at unit distance. The tangent, not the angle: the frame is flat and the sphere
  // is not, so spacing a marker by angle would bunch everything toward the edges.
  const halfWidth = Math.tan((lens.horizontalFovDeg / 2) * DEG_TO_RAD);
  const halfHeight = Math.tan((lens.verticalFovDeg / 2) * DEG_TO_RAD);
  if (!(halfWidth > 0) || !(halfHeight > 0)) return { onScreen: false, bearingDeg };

  const across = seen.x / ahead / halfWidth;
  const up = seen.y / ahead / halfHeight;
  if (Math.abs(across) > 1 || Math.abs(up) > 1) return { onScreen: false, bearingDeg };

  // Into the frame the page draws in: x grows right, y grows *down*, both fractions of the
  // viewfinder. Flipping y here rather than at the point of use keeps the one sign conversion in
  // one place, which is the whole reason this module exists.
  return { onScreen: true, x: 0.5 + across / 2, y: 0.5 - up / 2, bearingDeg };
}
