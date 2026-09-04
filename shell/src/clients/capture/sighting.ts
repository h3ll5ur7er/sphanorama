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
  | {
      readonly onScreen: true; readonly x: number; readonly y: number;
      readonly bearingDeg: number; readonly awayDeg: number;
    }
  | { readonly onScreen: false; readonly bearingDeg: number; readonly awayDeg: number };

/**
 * The camera frame, and the box the page paints it into.
 *
 * These are not the same shape and `object-fit: cover` does not make them one: it scales the frame
 * until it fills the box and crops whatever hangs over. A marker is a fraction of the *frame*, so
 * drawing it as a fraction of the *box* puts it somewhere the thing it names is not — by about a
 * tenth of the screen at the edges of a phone camera in a portrait window.
 */
export interface ViewfinderFit {
  frameWidth: number;
  frameHeight: number;
  boxWidth: number;
  boxHeight: number;
}

/**
 * A point in the camera frame, as a fraction of the box on screen.
 *
 * An unmeasured frame or box passes the point straight through. Zero there means "not known yet"
 * — the first samples arrive before the video reports a size and before layout has run — and a
 * scale derived from it would fling every marker off the screen.
 */
export function intoViewfinder(x: number, y: number, fit: ViewfinderFit): { x: number; y: number } {
  const measured = fit.frameWidth > 0 && fit.frameHeight > 0
                   && fit.boxWidth > 0 && fit.boxHeight > 0;
  if (!measured) return { x, y };

  // What `cover` does, in one number per axis: the frame is scaled by whichever ratio is larger,
  // so the shown size is at least the box on both axes and larger than it on the cropped one.
  const scale = Math.max(fit.boxWidth / fit.frameWidth, fit.boxHeight / fit.frameHeight);
  return {
    x: 0.5 + (x - 0.5) * ((fit.frameWidth * scale) / fit.boxWidth),
    y: 0.5 + (y - 0.5) * ((fit.frameHeight * scale) / fit.boxHeight),
  };
}

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
  // How far the phone has to turn to face it, which the offset across the picture cannot say: a
  // cell 20 degrees to the right and one 160 degrees to the right point exactly the same way.
  // Both directions are unit vectors, so the dot product with straight ahead is just -z.
  const awayDeg = Math.acos(Math.min(1, Math.max(-1, -seen.z))) * RAD_TO_DEG;

  // How far ahead it is. Zero or less is beside or behind the camera, where a perspective divide
  // has no answer — not a small one, a wrong one, because the sign flips the picture.
  const ahead = -seen.z;
  if (ahead <= 0) return { onScreen: false, bearingDeg, awayDeg };

  // Half the frame at unit distance. The tangent, not the angle: the frame is flat and the sphere
  // is not, so spacing a marker by angle would bunch everything toward the edges.
  const halfWidth = Math.tan((lens.horizontalFovDeg / 2) * DEG_TO_RAD);
  const halfHeight = Math.tan((lens.verticalFovDeg / 2) * DEG_TO_RAD);
  if (!(halfWidth > 0) || !(halfHeight > 0)) return { onScreen: false, bearingDeg, awayDeg };

  const across = seen.x / ahead / halfWidth;
  const up = seen.y / ahead / halfHeight;
  if (Math.abs(across) > 1 || Math.abs(up) > 1) return { onScreen: false, bearingDeg, awayDeg };

  // Into the frame the page draws in: x grows right, y grows *down*, both fractions of the
  // viewfinder. Flipping y here rather than at the point of use keeps the one sign conversion in
  // one place, which is the whole reason this module exists.
  return { onScreen: true, x: 0.5 + across / 2, y: 0.5 - up / 2, bearingDeg, awayDeg };
}
