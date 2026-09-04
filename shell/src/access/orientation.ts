/**
 * Platform attitude readings, translated into the frame the core plans in.
 *
 * Three conventions meet here and none is negotiable. `DeviceOrientationEvent` reports intrinsic
 * Z-X'-Y'' Tait-Bryan angles against an east-north-up earth frame; `AbsoluteOrientationSensor`
 * reports the same rotation as a quaternion in `[x, y, z, w]` order; and the core plans in a Y-up
 * frame whose -Z is forward, which is what `FromAzimuthElevation` and `Direction` are written
 * against.
 *
 * Both platform readings describe the *chassis*, and the app is not aimed with the chassis — it is
 * aimed with the viewfinder, which the browser re-orients under the user when the phone is turned.
 * `screen.orientation.angle` is the difference, and applying it here is what makes the core's +X
 * axis mean "the right edge of the picture" (ADR 0017).
 *
 * Doing all of this in the browser adapter is deliberate: which angles a platform reports is V10,
 * so a second platform reporting a rotation matrix or a replayed log changes this file and nothing
 * above it.
 */
import type { ImuSample, Quat, Vec3 } from '../../../contracts/ts/contracts';
import type { OrientationSample } from './motion';

const DEG_TO_RAD = Math.PI / 180;

/** Earth (east-north-up) into the core's frame: -90° about east, so up becomes +Y. */
const EARTH_TO_CORE: Quat = { w: Math.SQRT1_2, x: -Math.SQRT1_2, y: 0, z: 0 };

function multiply(a: Quat, b: Quat): Quat {
  return {
    w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
    y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
    z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
  };
}

/**
 * The chassis attitude as the viewfinder's, in the core's frame.
 *
 * The screen turns under the device by `-angle` about the axis out of the display, and that axis
 * is the one the camera looks along — so this changes the horizon and provably cannot change the
 * aim, which is the property the tests pin down. Post-multiplied because the screen's rotation is
 * expressed in the device's own frame, not the earth's.
 */
function toCoreFrame(device: Quat, screenAngleDeg: number): Quat {
  const half = (screenAngleDeg * DEG_TO_RAD) / 2;
  const screen: Quat = { w: Math.cos(half), x: 0, y: 0, z: -Math.sin(half) };
  return multiply(EARTH_TO_CORE, multiply(device, screen));
}

/** A vector turned by a quaternion: q ⊗ v ⊗ q*. */
function rotate(q: Quat, v: Vec3): Vec3 {
  const t = multiply(q, { w: 0, x: v.x, y: v.y, z: v.z });
  const out = multiply(t, { w: q.w, x: -q.x, y: -q.y, z: -q.z });
  return { x: out.x, y: out.y, z: out.z };
}

/**
 * `DeviceMotionEvent.rotationRate` as the viewfinder's angular velocity, in rad/s.
 *
 * Three conversions, and each of them is a mistake someone will otherwise make later.
 *
 * **The axes are named, not ordered.** The platform reports `alpha` about the device's Z, `beta`
 * about X and `gamma` about Y — the same axes the orientation triple is composed from and in a
 * different order from how they are written. Reading the three numbers positionally puts the
 * yaw rate on the pitch axis.
 *
 * **The units are degrees per second** and the contract's are radians. A factor of 57 makes a
 * filter look broken rather than mis-scaled.
 *
 * **The screen rotation applies here too, inverted.** The attitude is turned into the
 * viewfinder's by post-multiplying the screen rotation (see `toCoreFrame`), and a rate is a
 * body-frame vector: post-multiplying the attitude by `s` re-expresses body vectors through
 * `s⁻¹`. So a rate about the chassis' long axis is a rate about a different axis of the picture,
 * and in landscape — which is how this app is actually held — it is a different axis by 90°.
 *
 * Only the screen rotation appears. `EARTH_TO_CORE` is a change of *world* frame and a body-frame
 * rate does not see it, which is why the pose engine can post-multiply this straight onto an
 * attitude.
 */
export function angularVelocityFromRotationRate(
  alphaDegPerSec: number, betaDegPerSec: number, gammaDegPerSec: number, screenAngleDeg: number,
): Vec3 {
  const device: Vec3 = {
    x: betaDegPerSec * DEG_TO_RAD,
    y: gammaDegPerSec * DEG_TO_RAD,
    z: alphaDegPerSec * DEG_TO_RAD,
  };
  const half = (screenAngleDeg * DEG_TO_RAD) / 2;
  const inverseScreen: Quat = { w: Math.cos(half), x: 0, y: 0, z: Math.sin(half) };
  return rotate(inverseScreen, device);
}

/**
 * A DeviceOrientation triple as the viewfinder's attitude: apply it to -Z and you get the
 * direction the camera is looking, in the same frame the plan's target orientations live in.
 *
 * The screen angle is a required argument rather than an optional one because forgetting it is
 * not a visible mistake — the reticle still tracks, and only the horizon is a quarter turn out.
 */
export function quaternionFromDeviceOrientation(
  alphaDeg: number, betaDeg: number, gammaDeg: number, screenAngleDeg: number,
): Quat {
  const z = (alphaDeg * DEG_TO_RAD) / 2;
  const x = (betaDeg * DEG_TO_RAD) / 2;
  const y = (gammaDeg * DEG_TO_RAD) / 2;

  const cX = Math.cos(x), sX = Math.sin(x);
  const cY = Math.cos(y), sY = Math.sin(y);
  const cZ = Math.cos(z), sZ = Math.sin(z);

  // Z ⊗ X ⊗ Y, expanded — the composition the DeviceOrientation spec defines. Composing three
  // axis-angle quaternions would be clearer and is measurably slower at sensor rate.
  const device: Quat = {
    w: cX * cY * cZ - sX * sY * sZ,
    x: sX * cY * cZ - cX * sY * sZ,
    y: cX * sY * cZ + sX * cY * sZ,
    z: cX * cY * sZ + sX * sY * cZ,
  };

  return toCoreFrame(device, screenAngleDeg);
}

/**
 * The same thing from a Generic Sensor reading, which arrives already composed.
 *
 * `OrientationSensor.quaternion` is `[x, y, z, w]` — the opposite end first from how a quaternion
 * is usually written — and is read against the device reference frame, so it is the same rotation
 * the Euler triple describes and goes through the same conversion.
 */
export function quaternionFromSensorReading(
  reading: ArrayLike<number>, screenAngleDeg: number,
): Quat {
  const device: Quat = {
    w: reading[3], x: reading[0], y: reading[1], z: reading[2],
  };
  return toCoreFrame(device, screenAngleDeg);
}

/**
 * A motion sample as the contract's IMU sample.
 *
 * Both flags are the same idea and both matter: `hasOrientation` says the attitude is real
 * (ADR 0015) and `hasAngularVelocity` says the rate is (ADR 0025). Zero is a real rate, so a
 * struct field alone cannot say whether the device was still or nothing measured it — and the
 * engine fuses or does not fuse on exactly that distinction.
 *
 * Acceleration and the magnetometer are still unfilled and still say so.
 */
export function toImuSample(sample: OrientationSample): ImuSample {
  const zero = { x: 0, y: 0, z: 0 };
  return {
    timestampNs: sample.timestampNs,
    angularVelocity: sample.angularVelocity ?? zero,
    hasAngularVelocity: sample.angularVelocity !== undefined,
    acceleration: zero,
    hasMagnetometer: false,
    magneticField: zero,
    hasOrientation: true,
    orientation: sample.orientation,
  };
}
