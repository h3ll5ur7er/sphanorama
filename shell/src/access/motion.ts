/**
 * Browser implementation of IMotionSensorAccess.
 *
 * All the platform's awkwardness lives here by design: iOS gates motion behind a grant that must
 * follow a user gesture and rejects rather than resolving when it does not, Android exposes the
 * events with no gate at all, and desktops often have neither. Above this file, motion is a
 * capability and a stream of samples in the core's frame.
 *
 * Two sources, one sample. `AbsoluteOrientationSensor` reports a quaternion and is preferred;
 * `deviceorientation` reports an Euler triple whose parameterisation is degenerate in this app's
 * primary pose, and is the fallback (ADR 0017).
 */
import type { Quat } from '../../../contracts/ts/contracts';
import { err, ok, type Result } from './result';
import { quaternionFromDeviceOrientation, quaternionFromSensorReading } from './orientation';

const COMPONENT = 'MotionSensorAccess';

// Orientation is only useful while it is current: if the capture loop stalls, stale samples are
// worthless and an unbounded queue is a way to kill the tab.
const MAX_BUFFERED_SAMPLES = 512;

export type MotionCapability = 'None' | 'OrientationOnly' | 'GyroAccel' | 'GyroAccelMag';

/** Which of the two platform APIs is live. Reported so a phone can be diagnosed from its screen. */
export type MotionSource = 'none' | 'AbsoluteOrientationSensor' | 'DeviceOrientation';

export interface OrientationSample {
  timestampNs: number;
  /** The viewfinder's attitude in the core's frame — already converted (ADR 0017). */
  orientation: Quat;
}

export interface MotionSensorAccess {
  capabilities(): Promise<Result<MotionCapability>>;
  source(): MotionSource;
  start(requestedHz: number): Promise<Result<void>>;
  drain(max: number): Promise<Result<OrientationSample[]>>;
  stop(): Promise<Result<void>>;
}

/**
 * The bits of `window` this adapter uses, so tests can supply them without a DOM.
 *
 * The constructors are `unknown` on purpose: `requestPermission` is an iOS-only extension that the
 * DOM lib does not declare, and `AbsoluteOrientationSensor` is absent from it entirely. Both are
 * narrowed where they are used rather than asserted here against a type the platform disagrees
 * with.
 */
export interface MotionWindow {
  DeviceOrientationEvent?: unknown;
  DeviceMotionEvent?: unknown;
  AbsoluteOrientationSensor?: unknown;
  screen?: unknown;
  addEventListener(type: string, listener: (event: Event) => void): void;
  removeEventListener(type: string, listener: (event: Event) => void): void;
}

type PermissionGate = { requestPermission: () => Promise<string> };

/** The shape of an OrientationSensor, which delivers by mutating itself and firing an event. */
interface OrientationSensorLike {
  quaternion: ArrayLike<number> | null;
  timestamp: number | null;
  addEventListener(type: string, listener: () => void): void;
  start(): void;
  stop(): void;
}

type OrientationSensorCtor =
  new (options: { frequency: number; referenceFrame: string }) => OrientationSensorLike;

function permissionGate(candidate: unknown): PermissionGate | null {
  return typeof (candidate as PermissionGate | undefined)?.requestPermission === 'function'
    ? (candidate as PermissionGate)
    : null;
}

export function createMotionSensorAccess(host: MotionWindow): MotionSensorAccess {
  let buffered: OrientationSample[] = [];
  let live: MotionSource = 'none';
  let listener: ((event: Event) => void) | null = null;
  let sensor: OrientationSensorLike | null = null;

  /**
   * How far the page has been rotated under the device, read per sample rather than cached.
   *
   * The user turns the phone mid-session; an angle taken once at start would leave the horizon a
   * quarter turn out until the next capture. A host with no Screen Orientation API is unrotated,
   * which is true of every desktop and of the fakes.
   */
  function screenAngleDeg(): number {
    const orientation = (host.screen as { orientation?: { angle?: number } } | undefined)
      ?.orientation;
    return typeof orientation?.angle === 'number' ? orientation.angle : 0;
  }

  function push(sample: OrientationSample) {
    buffered.push(sample);
    if (buffered.length > MAX_BUFFERED_SAMPLES) {
      buffered = buffered.slice(-MAX_BUFFERED_SAMPLES);
    }
  }

  function detect(): MotionCapability {
    // What this adapter actually delivers, not what the platform could deliver. Both sources
    // report a fused attitude and no rates — OrientationOnly is the honest description of that
    // stream even on a device with a gyroscope.
    //
    // Claiming GyroAccel because DeviceMotionEvent exists would tell the core it has angular
    // velocity: PoseEngine would pick a fusion mode for rates that never arrive, and Stability,
    // computed from the zeros standing in for them, would report a phone mid-swing as perfectly
    // still. Reporting GyroAccel is a job for the day rotationRate is adapted too.
    if (host.AbsoluteOrientationSensor || host.DeviceOrientationEvent) return 'OrientationOnly';
    return 'None';
  }

  async function requestPermissionIfGated(): Promise<Result<void>> {
    const gate = permissionGate(host.DeviceOrientationEvent);
    if (!gate) return ok(undefined);
    try {
      const outcome = await gate.requestPermission();
      return outcome === 'granted'
        ? ok(undefined)
        : err('SensorPermissionDenied', COMPONENT, `permission ${outcome}`);
    } catch (cause) {
      // Safari rejects when the call did not follow a user gesture. From the capture client's
      // point of view that is the same outcome as a decline, and it reacts the same way.
      return err('SensorPermissionDenied', COMPONENT, String(cause));
    }
  }

  /**
   * Tears the sensor down and gives up the claim on it in the same breath.
   *
   * Both parts, because a stopped sensor still named as the live source is worse than either on
   * its own: `drain` would go on succeeding against a buffer nothing fills, and the readout would
   * name a sensor that has been stopped. Whatever takes over — the fallback below, or nothing at
   * all when the platform has no orientation event to fall back to — says so itself.
   */
  function stopSensor() {
    live = 'none';
    if (!sensor) return;
    try { sensor.stop(); } catch { /* already dead; nothing here can act on it */ }
    sensor = null;
  }

  /**
   * Starts the quaternion sensor, or reports that there is none to start.
   *
   * A sensor can fail two ways and only one of them is synchronous: the constructor throws a
   * SecurityError where the permissions policy forbids it, and `start()` reports a missing
   * gyroscope or a refused grant later, through an error event. Both land on the fallback.
   */
  function startSensor(requestedHz: number, onFailure: () => void): boolean {
    const ctor = host.AbsoluteOrientationSensor as OrientationSensorCtor | undefined;
    if (!ctor) return false;

    try {
      // The device reference frame rather than 'screen': the screen rotation is applied in
      // orientation.ts, so both sources go through one implementation of it and the tests can
      // hold them to the same answer. Asking the platform to do it for one source only would put
      // the rule in two places, and the platform's half would be untestable here.
      const started = new ctor({ frequency: requestedHz, referenceFrame: 'device' });
      started.addEventListener('reading', () => {
        const reading = started.quaternion;
        // A reading event with no quaternion is what a sensor fires before its first fix.
        if (!reading || reading.length < 4) return;
        push({
          timestampNs: Math.round((started.timestamp ?? 0) * 1e6),
          orientation: quaternionFromSensorReading(reading, screenAngleDeg()),
        });
      });
      started.addEventListener('error', () => {
        stopSensor();
        onFailure();
      });
      // Claimed alongside being installed rather than by the caller afterwards, so there is one
      // place that knows what is live — start() is where a sensor that cannot run says so, and
      // the fallback it triggers is entitled to overwrite this.
      sensor = started;
      live = 'AbsoluteOrientationSensor';
      started.start();
      return true;
    } catch {
      stopSensor();
      return false;
    }
  }

  async function startEvents(): Promise<Result<void>> {
    if (!host.DeviceOrientationEvent) {
      return err('SensorUnavailable', COMPONENT, 'no orientation events on this device');
    }
    const permitted = await requestPermissionIfGated();
    if (!permitted.ok) return permitted;

    listener = (rawEvent: Event) => {
      const event = rawEvent as DeviceOrientationEvent;
      const { alpha, beta, gamma } = event;
      // The angles are nullable, and a browser that cannot produce an attitude fires the event
      // with nulls rather than not firing at all. Coercing those to zero would hand the core a
      // fabricated attitude — level, facing north — carrying the confidence of a measurement
      // (ADR 0015), which is worse than no sample: a missing sample is a dropout the pose
      // engine already handles, and a wrong one aims the reticle at the wrong cell.
      //
      // Zero is a real angle; only null means unavailable, so this tests for null and not for
      // falsiness.
      if (alpha === null || beta === null || gamma === null ||
          alpha === undefined || beta === undefined || gamma === undefined) {
        return;
      }
      push({
        timestampNs: Math.round((event.timeStamp ?? 0) * 1e6),
        orientation: quaternionFromDeviceOrientation(alpha, beta, gamma, screenAngleDeg()),
      });
    };

    host.addEventListener('deviceorientation', listener);
    live = 'DeviceOrientation';
    return ok(undefined);
  }

  return {
    async capabilities() {
      return ok(detect());
    },

    source() {
      return live;
    },

    async start(requestedHz: number) {
      if (requestedHz <= 0) {
        return err('InvalidArgument', COMPONENT, 'sample rate must be positive');
      }
      if (detect() === 'None') {
        return err('SensorUnavailable', COMPONENT, 'no motion sensors on this device');
      }

      // The permission gate is only reached on the fallback path, and only iOS has one — where
      // there is no Generic Sensor API, so the gated call still happens inside the user gesture
      // that started this. A sensor that fails later falls back outside the gesture, on a
      // platform that does not gate.
      if (startSensor(requestedHz, () => { void startEvents(); })) return ok(undefined);
      return startEvents();
    },

    async drain(max: number) {
      if (live === 'none') return err('FailedPrecondition', COMPONENT, 'sensor is not running');
      const taken = buffered.slice(0, Math.max(0, max));
      buffered = buffered.slice(taken.length);
      return ok(taken);
    },

    async stop() {
      stopSensor();
      if (listener) host.removeEventListener('deviceorientation', listener);
      listener = null;
      live = 'none';
      buffered = [];
      return ok(undefined);
    },
  };
}
