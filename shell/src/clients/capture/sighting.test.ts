// Where a cell falls in the viewfinder, and which way to turn when it does not fall there at all.
//
// Every assertion is a decision the overlay makes — on screen or not, where, which way to turn —
// and none of them is about how a marker looks, which is reviewed by eye. The screen frame is the
// one thing worth stating twice: x grows right, y grows *down*, both as fractions of the
// viewfinder, so that a sighting can be handed to CSS without a second conversion inventing a
// sign of its own.
import { describe, expect, it } from 'vitest';

import { intoViewfinder, sight } from './sighting';
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

/** The attitude of a phone turned to look at a cell that far away, used as the cell's own. */
const turned = attitude;

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

describe('fitting the frame into the box it is painted in', () => {
  // `object-fit: cover` scales the video to fill its box and crops what does not fit. Markers are
  // fractions of the *camera frame*, so drawing them as fractions of the box puts every one of
  // them somewhere the feature it names is not.
  const box = { boxWidth: 900, boxHeight: 1650 };

  it('leaves a point alone when the frame and the box are the same shape', () => {
    const square = { frameWidth: 100, frameHeight: 100, boxWidth: 400, boxHeight: 400 };
    expect(intoViewfinder(0.25, 0.75, square)).toEqual({ x: 0.25, y: 0.75 });
  });

  it('spreads a point outward on the axis the fit cropped', () => {
    // A 960×1280 phone camera in a box taller than it is: cover fills the height and cuts about a
    // quarter off the width, so the right-hand edge of the frame is off screen and everything
    // still visible is further from the middle than its fraction of the frame says.
    const fit = { frameWidth: 960, frameHeight: 1280, ...box };
    const seen = intoViewfinder(0.75, 0.5, fit);

    expect(seen.y).toBeCloseTo(0.5, 6);
    expect(seen.x).toBeGreaterThan(0.75);
  });

  it('keeps the middle of the frame in the middle of the box', () => {
    // Whatever the crop, the centre is the one point that cannot move: it is what the reticle
    // marks, and a marker for the cell being aimed at has to land on it.
    const fit = { frameWidth: 960, frameHeight: 1280, ...box };
    expect(intoViewfinder(0.5, 0.5, fit)).toEqual({ x: 0.5, y: 0.5 });
  });

  it('crops the other axis when the box is the wider one', () => {
    const fit = { frameWidth: 1280, frameHeight: 960, boxWidth: 1600, boxHeight: 600 };
    const seen = intoViewfinder(0.5, 0.75, fit);

    expect(seen.x).toBeCloseTo(0.5, 6);
    expect(seen.y).toBeGreaterThan(0.75);
  });

  it('passes the point through rather than dividing by a box nothing has measured yet', () => {
    // The first frames arrive before the video has a size and before layout has run. A zero here
    // is "not known yet", and inventing a scale from it would fling every marker to infinity.
    const unmeasured = { frameWidth: 0, frameHeight: 0, boxWidth: 900, boxHeight: 1650 };
    expect(intoViewfinder(0.3, 0.4, unmeasured)).toEqual({ x: 0.3, y: 0.4 });
    expect(intoViewfinder(0.3, 0.4, { frameWidth: 960, frameHeight: 1280, boxWidth: 0, boxHeight: 0 }))
      .toEqual({ x: 0.3, y: 0.4 });
  });
});

describe('how far away a sighting is', () => {
  it('is zero for the cell you are looking straight at', () => {
    expect(sight(ahead, ahead, LENS).awayDeg).toBeCloseTo(0, 6);
  });

  it('is the angle you have to turn through, not the offset across the picture', () => {
    // The arrow's whole problem: a cell 20° to the right and one 160° to the right point the same
    // way. Without this the user turns and turns and nothing on screen says how much is left.
    expect(sight(ahead, turned(20, 0), LENS).awayDeg).toBeCloseTo(20, 4);
    expect(sight(ahead, turned(160, 0), LENS).awayDeg).toBeCloseTo(160, 4);
  });

  it('reaches 180 for the cell directly behind you', () => {
    expect(sight(ahead, turned(180, 0), LENS).awayDeg).toBeCloseTo(180, 4);
  });
});
