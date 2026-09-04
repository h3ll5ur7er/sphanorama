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
import type { Quat, Vec3 } from '../../../contracts/ts/contracts';
import { err, ok, type Result, type Status } from './result';
import {
  angularVelocityFromRotationRate, quaternionFromDeviceOrientation, quaternionFromSensorReading,
} from './orientation';

const COMPONENT = 'MotionSensorAccess';

// Orientation is only useful while it is current: if the capture loop stalls, stale samples are
// worthless and an unbounded queue is a way to kill the tab.
const MAX_BUFFERED_SAMPLES = 512;

// How far apart in time a rate and an attitude may be and still describe the same instant. Two
// streams arrive independently, so the rate riding along with a sample is always a little out; a
// fifth of a second is several sensor periods of slack and far less than the time it takes a hand
// to change direction. Past it the sample goes without rates rather than carrying a motion that
// belongs to a different moment, which the engine would correct against and could not detect.
//
// Applied in both directions. It was one-sided at first, on the reasoning that a rate *newer* than
// the attitude is just the two streams interleaving — true of the few milliseconds of skew between
// two events on the same clock, and no reason to accept any amount of it. A rate five seconds in
// the future is mis-associated for exactly the same reason one five seconds old is.
const MAX_RATE_SKEW_MS = 200;

export type MotionCapability = 'None' | 'OrientationOnly' | 'GyroAccel' | 'GyroAccelMag';

/** Which of the two platform APIs is live. Reported so a phone can be diagnosed from its screen. */
export type MotionSource = 'none' | 'AbsoluteOrientationSensor' | 'DeviceOrientation';

export interface OrientationSample {
  timestampNs: number;
  /** The viewfinder's attitude in the core's frame — already converted (ADR 0017). */
  orientation: Quat;
  /**
   * The gyroscope's rate at that moment, in rad/s in the same frame, when the platform measured
   * one recently enough to describe this attitude.
   *
   * Absent rather than zeroed, because zero is a real rate: a sample that cannot tell "still"
   * from "nobody looked" is the sample that reports a phone mid-swing as perfectly still. It
   * becomes `hasAngularVelocity` on the contract's ImuSample (ADR 0025).
   */
  angularVelocity?: Vec3;
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
  let motionListener: ((event: Event) => void) | null = null;
  let sensor: OrientationSensorLike | null = null;
  // Why nothing is running, when something used to be.
  //
  // The quaternion sensor reports a missing gyroscope or a refused grant asynchronously, long
  // after start() returned ok, and the fallback it hands over to can fail on its own — with no
  // caller left to return to. Kept here and handed out by drain(), which the capture loop reads
  // every tick: without it the phone simply stops tracking, and the only thing on screen is a
  // source that has quietly gone to 'none'.
  let lost: Status | null = null;
  // The most recent measured rate, in the chassis frame the platform reported it in, and when it
  // was measured — in the platform's own milliseconds, so it can be compared with the event
  // timestamps the attitudes carry.
  //
  // Unconverted on purpose. The screen angle is read per sample because the user turns the phone
  // mid-session, so a rate converted when it arrived is frozen in the frame of that moment: a
  // sample straddling a rotation would carry an attitude and a rate ninety degrees apart, and the
  // engine would correct along an axis the device is not turning about. It is converted when it
  // is attached, against the same angle the attitude used.
  let latestRate: { alpha: number; beta: number; gamma: number; atMs: number } | null = null;

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

  /**
   * The rate to attach to an attitude taken at this moment, if there is a fresh one.
   *
   * Attached rather than carried on its own sample because fusion wants a prediction and a
   * reading describing the same instant; a rate on a sample of its own would be integrated on one
   * tick and corrected on the next, which is the lag this is supposed to remove.
   */
  function rateFor(timestampNs: number, screenAngleDeg: number): Vec3 | undefined {
    if (!latestRate) return undefined;
    const skewMs = Math.abs(timestampNs / 1e6 - latestRate.atMs);
    if (skewMs > MAX_RATE_SKEW_MS) return undefined;
    return angularVelocityFromRotationRate(
      latestRate.alpha, latestRate.beta, latestRate.gamma, screenAngleDeg);
  }

  function push(sample: OrientationSample, screenAngleDeg: number) {
    const rate = rateFor(sample.timestampNs, screenAngleDeg);
    buffered.push(rate ? { ...sample, angularVelocity: rate } : sample);
    if (buffered.length > MAX_BUFFERED_SAMPLES) {
      buffered = buffered.slice(-MAX_BUFFERED_SAMPLES);
    }
  }

  function detect(): MotionCapability {
    // What the platform can report. This used to say OrientationOnly wherever an attitude was
    // available, because claiming GyroAccel with no rates adapted would have told the core it had
    // angular velocity it was never going to get — and a zeroed rate read as a measurement is a
    // phone mid-swing reported as perfectly still.
    //
    // The rates are adapted now, and more to the point each sample says for itself whether its
    // rate was measured (ADR 0025). So the capability is free to describe the platform again: an
    // over-claim costs nothing, because nothing reads it to decide whether a rate is real.
    if (host.DeviceMotionEvent && (host.AbsoluteOrientationSensor || host.DeviceOrientationEvent)) {
      return 'GyroAccel';
    }
    if (host.AbsoluteOrientationSensor || host.DeviceOrientationEvent) return 'OrientationOnly';
    return 'None';
  }

  /**
   * Asks for the motion grant, and reports whether there is one.
   *
   * Split from installing the listener because of *when* it has to be asked rather than what it
   * does: iOS requires each `requestPermission` to happen during a transient user activation, and
   * awaiting the first one spends that activation — so a second request made after it is rejected
   * unread. Both are therefore started before either is awaited, which is only possible if asking
   * and listening are separate steps.
   *
   * Deliberately not part of `start`'s success: iOS gates DeviceMotionEvent separately from
   * DeviceOrientationEvent, and a user who granted one and denied the other has a session that
   * works exactly as it did before rates existed. Failing the start over it would turn a lost
   * optimisation into a lost capture.
   */
  async function askForRates(): Promise<boolean> {
    if (!host.DeviceMotionEvent) return false;
    const gate = permissionGate(host.DeviceMotionEvent);
    if (!gate) return true;
    try {
      return await gate.requestPermission() === 'granted';
    } catch {
      // Safari rejects when the call did not follow a user gesture; same outcome as a decline.
      return false;
    }
  }

  function listenForRates(): void {
    motionListener = (rawEvent: Event) => {
      const event = rawEvent as DeviceMotionEvent;
      const rate = event.rotationRate;
      // A null rotationRate is what a desktop with no gyroscope fires, and a partial one is the
      // same rule the angles already follow: zero is a real rate, only null is unavailable, so a
      // missing axis is not completed with a zero.
      if (!rate || rate.alpha === null || rate.beta === null || rate.gamma === null ||
          rate.alpha === undefined || rate.beta === undefined || rate.gamma === undefined) {
        // Forgotten rather than left behind. An event that reports no complete rate is the
        // platform saying it has stopped, and keeping the last one alive until the staleness
        // window closes would hand the core a measurement that was explicitly withdrawn — marked
        // as measured, which is the one thing `hasAngularVelocity` exists to prevent.
        latestRate = null;
        return;
      }
      latestRate = {
        alpha: rate.alpha, beta: rate.beta, gamma: rate.gamma, atMs: event.timeStamp ?? 0,
      };
    };
    host.addEventListener('devicemotion', motionListener);
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
        // One read of the angle for both halves of the sample, so they cannot disagree about
        // which way up the picture is.
        const screen = screenAngleDeg();
        push({
          timestampNs: Math.round((started.timestamp ?? 0) * 1e6),
          orientation: quaternionFromSensorReading(reading, screen),
        }, screen);
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

  /**
   * Hands over to the orientation event after the quaternion sensor has died, and remembers it if
   * that fails too. The `error` listener has nowhere to return a Result to, so this is the seam
   * where one stops being a return value and becomes state drain() reports.
   */
  async function fallBackToEvents(): Promise<void> {
    const fallback = await startEvents();
    lost = fallback.ok ? null : fallback.status;
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
      const screen = screenAngleDeg();
      push({
        timestampNs: Math.round((event.timeStamp ?? 0) * 1e6),
        orientation: quaternionFromDeviceOrientation(alpha, beta, gamma, screen),
      }, screen);
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
      // Asked first and awaited last. Both grants have to be requested inside the user gesture
      // that reached this call, and awaiting either one ends it — so this starts the motion
      // request and lets the orientation request go out underneath it, then reads both answers.
      const rates = askForRates();

      // The attitude source is what a failed start reports on, and its listener stays the first
      // thing registered on the host.
      const started = startSensor(requestedHz, () => { void fallBackToEvents(); })
                          ? ok(undefined)
                          : await startEvents();
      if (started.ok && await rates) listenForRates();
      return started;
    },

    async drain(max: number) {
      if (live === 'none') {
        return lost
          ? err(lost.code, lost.component, lost.detail)
          : err('FailedPrecondition', COMPONENT, 'sensor is not running');
      }
      const taken = buffered.slice(0, Math.max(0, max));
      buffered = buffered.slice(taken.length);
      return ok(taken);
    },

    async stop() {
      stopSensor();
      if (listener) host.removeEventListener('deviceorientation', listener);
      if (motionListener) host.removeEventListener('devicemotion', motionListener);
      listener = null;
      motionListener = null;
      latestRate = null;
      live = 'none';
      // A deliberate stop is not a failure, and leaving the last one set would have drain blaming
      // a dead sensor for a session the caller ended itself.
      lost = null;
      buffered = [];
      return ok(undefined);
    },
  };
}
