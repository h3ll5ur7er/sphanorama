import { describe, expect, it } from 'vitest';
import { quaternionFromDeviceOrientation } from '../../access/orientation';
import { describeAttitude } from './attitude';

// The adapter is reached for to build the inputs, and only here: hand-written quaternion
// components would make every case unreadable and unfalsifiable by inspection, which is the
// reason none of the orientation tests assert on them either. The shipped client holds no such
// edge — it reads attitudes off the port.

/** The triple a level phone held in landscape reports, and the screen angle that goes with it. */
const LANDSCAPE_LEVEL: [number, number, number] = [90, 0, -90];

const from = (a: number, b: number, g: number, screen: number) =>
  describeAttitude(quaternionFromDeviceOrientation(a, b, g, screen));

describe('describeAttitude', () => {
  it('reads a phone held upright and facing north as level and on the horizon', () => {
    expect(from(0, 90, 0, 0)).toBe('az 0° el 0° roll 0°');
  });

  it('reads the compass round the horizon', () => {
    // alpha 90 turns the phone to the west, which is azimuth 90 in the plan's sense.
    expect(from(90, 90, 0, 0)).toBe('az 90° el 0° roll 0°');
    expect(from(180, 90, 0, 0)).toBe('az 180° el 0° roll 0°');
    // And wraps rather than going negative, so the readout does not jump by 360 as the user turns.
    expect(from(-90, 90, 0, 0)).toBe('az 270° el 0° roll 0°');
  });

  it('reads elevation out of the horizon in both directions', () => {
    expect(from(0, 180, 0, 0)).toContain('el 90°');
    expect(from(0, 0, 0, 0)).toContain('el -90°');
    expect(from(0, 135, 0, 0)).toContain('el 45°');
  });

  it('reads a level landscape phone as having no roll', () => {
    // The whole point of the screen compensation, in the readout the user is looking at while
    // holding the phone that way.
    expect(from(...LANDSCAPE_LEVEL, 90)).toBe('az 0° el 0° roll 0°');
  });

  it('reads the same phone as rolled a quarter turn when the page is still portrait', () => {
    expect(from(...LANDSCAPE_LEVEL, 0)).toBe('az 0° el 0° roll -90°');
  });

  it('signs roll the way RollBetween does, and turns with the page', () => {
    // Built from the same construction as the core's RollBetween — from the level cell's
    // horizontal axis to the camera's, about the viewing axis — because the readout sits next to
    // the guidance line. Two roll numbers of opposite sign next to each other would be read as a
    // bug in whichever one the reader trusted less.
    expect(from(0, 90, 0, 45)).toBe('az 0° el 0° roll 45°');
    expect(from(0, 90, 0, 315)).toBe('az 0° el 0° roll -45°');
    // The same quarter turn the landscape case above shows, from the other starting pose.
    expect(from(0, 90, 0, 90)).toBe('az 0° el 0° roll 90°');
  });

  it('says something finite when the camera points at a pole, where azimuth has no meaning', () => {
    // Straight up is a real pose — it is a cell in the plan — and a readout of NaN would be read
    // as the sensor having failed.
    expect(from(0, 180, 0, 0)).toMatch(/^az -?\d+° el -?\d+° roll -?\d+°$/);
  });
});
