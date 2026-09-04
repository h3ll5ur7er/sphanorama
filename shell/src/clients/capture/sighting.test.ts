// Where a cell falls in the viewfinder, and which way to turn when it does not fall there at all.
//
// Every assertion is a decision the overlay makes — on screen or not, where, which way to turn —
// and none of them is about how a marker looks, which is reviewed by eye. The screen frame is the
// one thing worth stating twice: x grows right, y grows *down*, both as fractions of the
// viewfinder, so that a sighting can be handed to CSS without a second conversion inventing a
// sign of its own.
import { describe, expect, it } from 'vitest';

import { sight } from './sighting';
import type { Quat } from '../../../../contracts/ts/contracts';

const LENS = { horizontalFovDeg: 66, verticalFovDeg: 52 };

/** A cell aimed by azimuth then elevation, as the core's FromAzimuthElevation builds one. */
function attitude(azimuthDeg: number, elevationDeg: number): Quat {
  const yaw = (azimuthDeg * Math.PI) / 360;
  const pitch = (elevationDeg * Math.PI) / 360;
  const [cy, sy] = [Math.cos(yaw), Math.sin(yaw)];
  const [cp, sp] = [Math.cos(pitch), Math.sin(pitch)];
  return { w: cy * cp, x: cy * sp, y: sy * cp, z: -sy * sp };
}

const ahead = attitude(0, 0);

describe('sight', () => {
  it('puts the cell you are looking at in the middle of the viewfinder', () => {
    const seen = sight(ahead, ahead, LENS);
    expect(seen.onScreen).toBe(true);
    if (!seen.onScreen) return;
    expect(seen.x).toBeCloseTo(0.5, 6);
    expect(seen.y).toBeCloseTo(0.5, 6);
  });

  it('puts a cell you turn right to reach on the right of the viewfinder', () => {
    // The handedness, asserted rather than assumed. Turning the phone right swings the looking
    // direction toward +X, which is *decreasing* azimuth in this project's convention — so a cell
    // to your right has a negative azimuth, and anything that renders azimuth onto a left-to-right
    // axis has to know that or it draws the world mirrored.
    const seen = sight(ahead, attitude(-20, 0), LENS);
    expect(seen.onScreen).toBe(true);
    if (!seen.onScreen) return;
    expect(seen.x).toBeGreaterThan(0.5);
    expect(sight(ahead, attitude(20, 0), LENS)).toMatchObject({ onScreen: true });
    const left = sight(ahead, attitude(20, 0), LENS);
    if (left.onScreen) expect(left.x).toBeLessThan(0.5);
  });

  it('puts a cell above you at the top, where y is smaller', () => {
    const seen = sight(ahead, attitude(0, 15), LENS);
    expect(seen.onScreen).toBe(true);
    if (!seen.onScreen) return;
    expect(seen.y).toBeLessThan(0.5);
    expect(seen.x).toBeCloseTo(0.5, 6);
  });

  it('reaches the edge of the frame at half the field of view', () => {
    // The edge is the definition of the lens, so this is what ties the overlay to the plan: both
    // read the same two angles, and a marker that drifted off the frame early would mean the
    // planner and the viewfinder disagreed about what the camera can see.
    const edge = sight(ahead, attitude(-LENS.horizontalFovDeg / 2, 0), LENS);
    expect(edge.onScreen).toBe(true);
    if (!edge.onScreen) return;
    expect(edge.x).toBeCloseTo(1, 6);
  });

  it('does not claim a cell is on screen once it is past the edge', () => {
    expect(sight(ahead, attitude(-LENS.horizontalFovDeg, 0), LENS).onScreen).toBe(false);
    expect(sight(ahead, attitude(0, LENS.verticalFovDeg), LENS).onScreen).toBe(false);
  });

  it('knows a cell behind you is not on screen, and still says which way to turn', () => {
    // The case the arrow exists for. A projection that divided by the forward distance without
    // checking its sign would place this one on screen, mirrored, and send the user away from it.
    const behind = sight(ahead, attitude(180, 0), LENS);
    expect(behind.onScreen).toBe(false);
    expect(Number.isFinite(behind.bearingDeg)).toBe(true);
  });

  it('points the way you would actually turn', () => {
    // Bearing is degrees clockwise from straight up, so 90 is right and 270 is left.
    const toTheRight = sight(ahead, attitude(-100, 0), LENS);
    expect(toTheRight.bearingDeg).toBeCloseTo(90, 4);
    const toTheLeft = sight(ahead, attitude(100, 0), LENS);
    expect(toTheLeft.bearingDeg).toBeCloseTo(270, 4);
    const above = sight(ahead, attitude(0, 80), LENS);
    expect(above.bearingDeg).toBeCloseTo(0, 4);
  });

  it('turns the picture with the phone rather than leaving it upright', () => {
    // Rolling the phone must roll the overlay with it, or the markers slide off the cells they
    // name the moment someone tilts their hands. A cell directly above, seen through a phone
    // rolled a quarter turn, is off to one side.
    const rolled: Quat = { w: Math.cos(Math.PI / 4), x: 0, y: 0, z: Math.sin(Math.PI / 4) };
    const seen = sight(rolled, attitude(0, 15), LENS);
    expect(seen.onScreen).toBe(true);
    if (!seen.onScreen) return;
    expect(Math.abs(seen.x - 0.5)).toBeGreaterThan(Math.abs(seen.y - 0.5));
  });
});
