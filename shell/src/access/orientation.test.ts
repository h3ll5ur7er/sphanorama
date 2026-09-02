import { describe, expect, it } from 'vitest';
import type { Quat } from '../../../contracts/ts/contracts';
import { quaternionFromDeviceOrientation, toImuSample } from './orientation';

/**
 * Where the camera looks: the device's -Z axis expressed in the core's frame. The assertions are
 * written against this rather than against quaternion components, because a component is not
 * something anyone can be wrong or right about by inspection.
 */
function look(q: Quat) {
  const { w, x, y, z } = q;
  return {
    x: -2 * (x * z + w * y),
    y: -2 * (y * z - w * x),
    z: -(1 - 2 * (x * x + y * y)),
  };
}

const near = (actual: number, expected: number) => expect(actual).toBeCloseTo(expected, 6);

function looksAt(q: Quat, x: number, y: number, z: number) {
  const d = look(q);
  near(d.x, x);
  near(d.y, y);
  near(d.z, z);
}

describe('quaternionFromDeviceOrientation', () => {
  it('reads a phone lying flat as a camera pointing at the floor', () => {
    // Every session starts here — the phone is on a table or in a hand before it is raised — and
    // getting it wrong would place the first reticle half a sphere from the user.
    looksAt(quaternionFromDeviceOrientation(0, 0, 0), 0, -1, 0);
  });

  it('reads a phone held upright as a camera pointing at the horizon', () => {
    looksAt(quaternionFromDeviceOrientation(0, 90, 0), 0, 0, -1);
  });

  it('reads a phone tilted back as a camera pointing at the sky', () => {
    looksAt(quaternionFromDeviceOrientation(0, 180, 0), 0, 1, 0);
  });

  it('turns compass heading into azimuth about the up axis', () => {
    // alpha grows counter-clockwise seen from above, which is the same sense as the plan's
    // azimuth. A sign error here shows up as a reticle that runs away from the user.
    looksAt(quaternionFromDeviceOrientation(90, 90, 0), -1, 0, 0);
    looksAt(quaternionFromDeviceOrientation(180, 90, 0), 0, 0, 1);
  });

  it('stays a unit quaternion for arbitrary attitudes', () => {
    // AngleBetween divides by the norms; drift here would show as guidance that grows steadily
    // more wrong the further the phone is from the axes the other cases pin down.
    for (const [a, b, g] of [[37, 12, -83], [-140, 61, 44], [359, -179, 89]]) {
      const q = quaternionFromDeviceOrientation(a, b, g);
      near(Math.hypot(q.w, q.x, q.y, q.z), 1);
    }
  });

  it('is a rotation, so the looking direction is always a unit vector', () => {
    for (const [a, b, g] of [[0, 45, 30], [200, -60, 170]]) {
      const d = look(quaternionFromDeviceOrientation(a, b, g));
      near(Math.hypot(d.x, d.y, d.z), 1);
    }
  });
});

describe('toImuSample', () => {
  it('marks the sample as carrying an absolute orientation', () => {
    // The flag is what tells PoseEngine it may use the orientation instead of integrating rates
    // it does not have. Without it the browser's samples would be silently ignored.
    const sample = toImuSample({ timestampNs: 1234, orientation: { alpha: 0, beta: 90, gamma: 0 } });
    expect(sample.hasOrientation).toBe(true);
    expect(sample.timestampNs).toBe(1234);
    looksAt(sample.orientation, 0, 0, -1);
  });

  it('reports no rates and no magnetometer rather than inventing zeros that read as measured', () => {
    const sample = toImuSample({ timestampNs: 0, orientation: { alpha: 10, beta: 20, gamma: 30 } });
    expect(sample.hasMagnetometer).toBe(false);
    expect(sample.angularVelocity).toEqual({ x: 0, y: 0, z: 0 });
  });
});
