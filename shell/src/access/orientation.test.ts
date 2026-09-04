import { describe, expect, it } from 'vitest';
import type { Quat } from '../../../contracts/ts/contracts';
import {
  angularVelocityFromRotationRate,
  quaternionFromDeviceOrientation, quaternionFromSensorReading, toImuSample,
} from './orientation';

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

/**
 * The viewfinder's right edge, same frame. This is the axis roll is measured against — the core
 * compares it with the target cell's horizontal — so it is what a level horizon means.
 */
function right(q: Quat) {
  const { w, x, y, z } = q;
  return { x: 1 - 2 * (y * y + z * z), y: 2 * (x * y + w * z), z: 2 * (x * z - w * y) };
}

const near = (actual: number, expected: number) => expect(actual).toBeCloseTo(expected, 6);

function pointsAt(v: { x: number; y: number; z: number }, x: number, y: number, z: number) {
  near(v.x, x);
  near(v.y, y);
  near(v.z, z);
}

const looksAt = (q: Quat, x: number, y: number, z: number) => pointsAt(look(q), x, y, z);

/**
 * An independent Z-X'-Y'' composition, built from three axis-angle quaternions rather than the
 * expanded product the adapter uses. It is the reference the sensor path is checked against, and
 * it re-derives the expansion in orientation.ts from a form that can be read at a glance.
 */
function axisAngle(x: number, y: number, z: number, deg: number): Quat {
  const half = (deg * Math.PI) / 360;
  const s = Math.sin(half);
  return { w: Math.cos(half), x: x * s, y: y * s, z: z * s };
}

function mul(a: Quat, b: Quat): Quat {
  return {
    w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
  };
}

/** What AbsoluteOrientationSensor reports for the same attitude: [x, y, z, w], device frame. */
function sensorReading(a: number, b: number, g: number): number[] {
  const q = mul(mul(axisAngle(0, 0, 1, a), axisAngle(1, 0, 0, b)), axisAngle(0, 1, 0, g));
  return [q.x, q.y, q.z, q.w];
}

describe('quaternionFromDeviceOrientation', () => {
  it('reads a phone lying flat as a camera pointing at the floor', () => {
    // Every session starts here — the phone is on a table or in a hand before it is raised — and
    // getting it wrong would place the first reticle half a sphere from the user.
    looksAt(quaternionFromDeviceOrientation(0, 0, 0, 0), 0, -1, 0);
  });

  it('reads a phone held upright as a camera pointing at the horizon', () => {
    looksAt(quaternionFromDeviceOrientation(0, 90, 0, 0), 0, 0, -1);
  });

  it('reads a phone tilted back as a camera pointing at the sky', () => {
    looksAt(quaternionFromDeviceOrientation(0, 180, 0, 0), 0, 1, 0);
  });

  it('turns compass heading into azimuth about the up axis', () => {
    // alpha grows counter-clockwise seen from above, which is the same sense as the plan's
    // azimuth. A sign error here shows up as a reticle that runs away from the user.
    looksAt(quaternionFromDeviceOrientation(90, 90, 0, 0), -1, 0, 0);
    looksAt(quaternionFromDeviceOrientation(180, 90, 0, 0), 0, 0, 1);
  });

  it('swings the camera sideways when a flat phone is tipped onto its edge', () => {
    // gamma turns about the device's own long axis, and until now every direction case here set
    // it to zero — leaving the one angle the user actually exercises while rolling the phone
    // asserted nowhere. Flat and tipped a quarter turn, the camera stops facing the floor and
    // looks along the horizon instead.
    looksAt(quaternionFromDeviceOrientation(0, 0, 90, 0), -1, 0, 0);
    looksAt(quaternionFromDeviceOrientation(0, 0, -90, 0), 1, 0, 0);
  });

  it('stays a unit quaternion for arbitrary attitudes', () => {
    // AngleBetween divides by the norms; drift here would show as guidance that grows steadily
    // more wrong the further the phone is from the axes the other cases pin down.
    for (const [a, b, g] of [[37, 12, -83], [-140, 61, 44], [359, -179, 89]]) {
      for (const screen of [0, 90, 180, 270]) {
        const q = quaternionFromDeviceOrientation(a, b, g, screen);
        near(Math.hypot(q.w, q.x, q.y, q.z), 1);
      }
    }
  });

  it('is a rotation, so the looking direction is always a unit vector', () => {
    for (const [a, b, g] of [[0, 45, 30], [200, -60, 170]]) {
      const d = look(quaternionFromDeviceOrientation(a, b, g, 0));
      near(Math.hypot(d.x, d.y, d.z), 1);
    }
  });
});

describe('the screen the user is holding, not the chassis', () => {
  // A phone held in landscape, level, camera facing north. The browser reports the attitude of
  // the chassis and knows nothing about which way up the page is; screen.orientation does.
  const landscapeLevel = [90, 0, -90] as const;

  it('reads a level landscape phone as having a level horizon', () => {
    const q = quaternionFromDeviceOrientation(...landscapeLevel, 90);
    looksAt(q, 0, 0, -1);
    // Horizontal, which is what makes the roll error against a level cell zero. The core measures
    // roll from this axis, so a vertical one is 90 degrees of roll that the user is not applying.
    pointsAt(right(q), 1, 0, 0);
  });

  it('would call that same phone rolled a quarter turn if the screen angle were ignored', () => {
    // The bug this compensation exists for, pinned so it cannot come back quietly: the chassis
    // right edge points at the sky in landscape, and the artificial horizon would stand on end.
    pointsAt(right(quaternionFromDeviceOrientation(...landscapeLevel, 0)), 0, 1, 0);
  });

  it('turns the viewfinder about the camera without moving where the camera looks', () => {
    // The screen angle is a rotation about the viewing axis, so it may change the horizon and may
    // never change the aim. If it could, rotating the phone in the hand would send the reticle to
    // a different cell.
    for (const [a, b, g] of [[0, 90, 0], [37, 12, -83], [200, -60, 170]]) {
      const aim = look(quaternionFromDeviceOrientation(a, b, g, 0));
      for (const screen of [90, 180, 270]) {
        pointsAt(look(quaternionFromDeviceOrientation(a, b, g, screen)), aim.x, aim.y, aim.z);
      }
    }
  });

  it('turns the horizon the whole way round as the screen angle does', () => {
    const upright = [0, 90, 0] as const;
    pointsAt(right(quaternionFromDeviceOrientation(...upright, 0)), 1, 0, 0);
    pointsAt(right(quaternionFromDeviceOrientation(...upright, 90)), 0, -1, 0);
    pointsAt(right(quaternionFromDeviceOrientation(...upright, 180)), -1, 0, 0);
    pointsAt(right(quaternionFromDeviceOrientation(...upright, 270)), 0, 1, 0);
  });
});

describe('quaternionFromSensorReading', () => {
  it('agrees with the Euler path on every attitude, which is what makes them interchangeable', () => {
    // Two sources, one frame. If they disagreed, guidance would jump the moment the adapter fell
    // back from the sensor to the event — and the fallback happens on a phone, mid-session.
    for (const [a, b, g] of [[0, 0, 0], [0, 90, 0], [37, 12, -83], [-140, 61, 44], [90, 0, -90]]) {
      for (const screen of [0, 90, 180, 270]) {
        const fromSensor = quaternionFromSensorReading(sensorReading(a, b, g), screen);
        const fromEuler = quaternionFromDeviceOrientation(a, b, g, screen);
        // Sign is not identity: q and -q are the same rotation, so compare what they do.
        pointsAt(look(fromSensor), look(fromEuler).x, look(fromEuler).y, look(fromEuler).z);
        pointsAt(right(fromSensor), right(fromEuler).x, right(fromEuler).y, right(fromEuler).z);
      }
    }
  });

  it('reads the reading in the order the Generic Sensor API writes it', () => {
    // [x, y, z, w], not [w, x, y, z]. Getting this wrong produces a plausible-looking rotation
    // that is wrong everywhere, which is the hardest kind to notice.
    looksAt(quaternionFromSensorReading(sensorReading(0, 90, 0), 0), 0, 0, -1);
    looksAt(quaternionFromSensorReading(sensorReading(0, 0, 0), 0), 0, -1, 0);
  });

  it('accepts the typed array the sensor actually hands over', () => {
    const reading = Float32Array.from(sensorReading(0, 90, 0));
    looksAt(quaternionFromSensorReading(reading, 0), 0, 0, -1);
  });
});

describe('toImuSample', () => {
  it('marks the sample as carrying an absolute orientation', () => {
    // The flag is what tells PoseEngine it may use the orientation instead of integrating rates
    // it does not have. Without it the browser's samples would be silently ignored.
    const orientation = quaternionFromDeviceOrientation(0, 90, 0, 0);
    const sample = toImuSample({ timestampNs: 1234, orientation });
    expect(sample.hasOrientation).toBe(true);
    expect(sample.timestampNs).toBe(1234);
    looksAt(sample.orientation, 0, 0, -1);
  });

  it('reports no rates and no magnetometer rather than inventing zeros that read as measured', () => {
    const sample = toImuSample({
      timestampNs: 0, orientation: quaternionFromDeviceOrientation(10, 20, 30, 0),
    });
    expect(sample.hasMagnetometer).toBe(false);
    expect(sample.angularVelocity).toEqual({ x: 0, y: 0, z: 0 });
    // And says so, which is the difference between a still device and one nobody measured.
    expect(sample.hasAngularVelocity).toBe(false);
  });

  it('carries a measured rate and marks it as measured', () => {
    const sample = toImuSample({
      timestampNs: 0,
      orientation: quaternionFromDeviceOrientation(0, 0, 0, 0),
      angularVelocity: { x: 0.1, y: 0.2, z: 0.3 },
    });
    expect(sample.hasAngularVelocity).toBe(true);
    expect(sample.angularVelocity).toEqual({ x: 0.1, y: 0.2, z: 0.3 });
  });

  it('marks a measured rate of exactly zero as measured', () => {
    // The case the flag exists for: a device held perfectly still reports zeros, and they are
    // worth as much as any other reading.
    const sample = toImuSample({
      timestampNs: 0,
      orientation: quaternionFromDeviceOrientation(0, 0, 0, 0),
      angularVelocity: { x: 0, y: 0, z: 0 },
    });
    expect(sample.hasAngularVelocity).toBe(true);
  });
});

describe('angularVelocityFromRotationRate', () => {
  /**
   * The body-frame rotation from `from` to `to`, as an axis-angle vector in radians.
   *
   * This is what the pose engine does with an attitude pair, so measuring the converted rate
   * against it is the only check that actually ties the two halves of a sample together. A rate
   * on the wrong axis is invisible in every other test: the reticle still tracks, because the
   * attitude is right, and only the correction the engine applies pulls the wrong way.
   */
  function rotationBetween(from: Quat, to: Quat): { x: number; y: number; z: number } {
    const conj = { w: from.w, x: -from.x, y: -from.y, z: -from.z };
    let error = {
      w: conj.w * to.w - conj.x * to.x - conj.y * to.y - conj.z * to.z,
      x: conj.w * to.x + conj.x * to.w + conj.y * to.z - conj.z * to.y,
      y: conj.w * to.y - conj.x * to.z + conj.y * to.w + conj.z * to.x,
      z: conj.w * to.z + conj.x * to.y - conj.y * to.x + conj.z * to.w,
    };
    if (error.w < 0) error = { w: -error.w, x: -error.x, y: -error.y, z: -error.z };
    const axis = Math.hypot(error.x, error.y, error.z);
    if (axis < 1e-12) return { x: 0, y: 0, z: 0 };
    const radians = 2 * Math.atan2(axis, error.w);
    return {
      x: (error.x / axis) * radians,
      y: (error.y / axis) * radians,
      z: (error.z / axis) * radians,
    };
  }

  /**
   * Turns one device axis at a known rate and checks the converted rate against the converted
   * attitudes either side of it. Both halves of a sample have to describe the same motion in the
   * same frame or the engine corrects along an axis the device is not turning about.
   */
  function agreesWithTheAttitudes(
    axis: 'alpha' | 'beta' | 'gamma', screenAngleDeg: number,
  ): void {
    const rateDegPerSec = 12;
    const seconds = 0.25;
    const at = (t: number) => quaternionFromDeviceOrientation(
      axis === 'alpha' ? rateDegPerSec * t : 0,
      axis === 'beta' ? rateDegPerSec * t : 0,
      axis === 'gamma' ? rateDegPerSec * t : 0,
      screenAngleDeg,
    );

    const measured = rotationBetween(at(0), at(seconds));
    const converted = angularVelocityFromRotationRate(
      axis === 'alpha' ? rateDegPerSec : 0,
      axis === 'beta' ? rateDegPerSec : 0,
      axis === 'gamma' ? rateDegPerSec : 0,
      screenAngleDeg,
    );

    expect(converted.x).toBeCloseTo(measured.x / seconds, 6);
    expect(converted.y).toBeCloseTo(measured.y / seconds, 6);
    expect(converted.z).toBeCloseTo(measured.z / seconds, 6);
  }

  it('agrees with how the attitude actually changes, on every axis', () => {
    agreesWithTheAttitudes('alpha', 0);
    agreesWithTheAttitudes('beta', 0);
    agreesWithTheAttitudes('gamma', 0);
  });

  it('agrees with the attitudes when the screen is rotated under the device too', () => {
    // The screen angle turns the viewfinder's axes under the chassis, so a rate reported about
    // the chassis is about a different axis of the picture. Getting this wrong is invisible in
    // portrait and wrong by 90 degrees in landscape, which is how the app is actually held.
    agreesWithTheAttitudes('alpha', 90);
    agreesWithTheAttitudes('beta', 90);
    agreesWithTheAttitudes('gamma', 90);
  });

  it('is in radians per second, not degrees', () => {
    // The platform reports degrees and the contract says rad/s. A factor of 57 between them is
    // the kind of mistake that makes a filter look broken rather than mis-scaled.
    const converted = angularVelocityFromRotationRate(180, 0, 0, 0);
    expect(Math.hypot(converted.x, converted.y, converted.z)).toBeCloseTo(Math.PI, 9);
  });
});
