/**
 * DeviceOrientation, translated into the frame the core plans in.
 *
 * Two conventions meet here and neither is negotiable. The browser reports intrinsic Z-X'-Y''
 * Tait-Bryan angles against an east-north-up earth frame, with the device's -Z axis pointing out
 * of its back — where the camera looks. The core plans in a Y-up frame whose -Z is forward, which
 * is what `FromAzimuthElevation` and `Direction` are written against.
 *
 * Doing this conversion in the browser adapter is deliberate: the Euler convention is a property
 * of the platform (V2), and a second platform reporting a rotation matrix or a game-rotation
 * vector should not be able to reach any layer above this one.
 */
import type { ImuSample, Quat } from '../../../contracts/ts/contracts';
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
 * The device's attitude as the core understands it: apply it to -Z and you get the direction the
 * camera is looking, in the same frame the coverage plan's target orientations live in.
 */
export function quaternionFromDeviceOrientation(
  alphaDeg: number, betaDeg: number, gammaDeg: number,
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

  return multiply(EARTH_TO_CORE, device);
}

/**
 * An orientation event as the contract's IMU sample.
 *
 * The browser gives a fused attitude and no rates at all, which is exactly what
 * MotionCapability::OrientationOnly describes. The zeroed rate fields are not measurements and
 * `hasOrientation` is what tells PoseEngine which half of the sample is real.
 */
export function toImuSample(sample: OrientationSample): ImuSample {
  const { alpha, beta, gamma } = sample.orientation;
  const zero = { x: 0, y: 0, z: 0 };
  return {
    timestampNs: sample.timestampNs,
    angularVelocity: zero,
    acceleration: zero,
    hasMagnetometer: false,
    magneticField: zero,
    hasOrientation: true,
    orientation: quaternionFromDeviceOrientation(alpha, beta, gamma),
  };
}
