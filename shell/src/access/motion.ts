/**
 * Browser implementation of IMotionSensorAccess.
 *
 * All the platform's awkwardness lives here by design: iOS gates motion behind a grant that must
 * follow a user gesture and rejects rather than resolving when it does not, Android exposes the
 * events with no gate at all, and desktops often have neither. Above this file, motion is a
 * capability and a stream of samples.
 */
import { err, ok, type Result } from './result';

const COMPONENT = 'MotionSensorAccess';

// Orientation is only useful while it is current: if the capture loop stalls, stale samples are
// worthless and an unbounded queue is a way to kill the tab.
const MAX_BUFFERED_SAMPLES = 512;

export type MotionCapability = 'None' | 'OrientationOnly' | 'GyroAccel' | 'GyroAccelMag';

export interface OrientationSample {
  timestampNs: number;
  orientation: { alpha: number; beta: number; gamma: number };
}

export interface MotionSensorAccess {
  capabilities(): Promise<Result<MotionCapability>>;
  start(requestedHz: number): Promise<Result<void>>;
  drain(max: number): Promise<Result<OrientationSample[]>>;
  stop(): Promise<Result<void>>;
}

/**
 * The bits of `window` this adapter uses, so tests can supply them without a DOM.
 *
 * The event constructors are `unknown` on purpose: `requestPermission` is an iOS-only extension
 * that the DOM lib does not declare, so it is narrowed where it is used rather than asserted here
 * against a type the platform disagrees with.
 */
export interface MotionWindow {
  DeviceOrientationEvent?: unknown;
  DeviceMotionEvent?: unknown;
  addEventListener(type: string, listener: (event: Event) => void): void;
  removeEventListener(type: string, listener: (event: Event) => void): void;
}

type PermissionGate = { requestPermission: () => Promise<string> };

function permissionGate(candidate: unknown): PermissionGate | null {
  return typeof (candidate as PermissionGate | undefined)?.requestPermission === 'function'
    ? (candidate as PermissionGate)
    : null;
}

export function createMotionSensorAccess(host: MotionWindow): MotionSensorAccess {
  let running = false;
  let buffered: OrientationSample[] = [];
  let listener: ((event: Event) => void) | null = null;

  function detect(): MotionCapability {
    // What this adapter actually delivers, not what the platform could deliver. It subscribes to
    // deviceorientation alone, so every sample carries a fused attitude and no rates —
    // OrientationOnly is the honest description of that stream even on a device with a gyroscope.
    //
    // Claiming GyroAccel because DeviceMotionEvent exists would tell the core it has angular
    // velocity: PoseEngine would pick a fusion mode for rates that never arrive, and Stability,
    // computed from the zeros standing in for them, would report a phone mid-swing as perfectly
    // still. Reporting GyroAccel is a job for the day rotationRate is adapted too.
    if (host.DeviceOrientationEvent) return 'OrientationOnly';
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

  return {
    async capabilities() {
      return ok(detect());
    },

    async start(requestedHz: number) {
      if (requestedHz <= 0) {
        return err('InvalidArgument', COMPONENT, 'sample rate must be positive');
      }
      if (detect() === 'None') {
        return err('SensorUnavailable', COMPONENT, 'no motion sensors on this device');
      }

      const permitted = await requestPermissionIfGated();
      if (!permitted.ok) return permitted;

      listener = (rawEvent: Event) => {
        const event = rawEvent as DeviceOrientationEvent;
        buffered.push({
          timestampNs: Math.round((event.timeStamp ?? 0) * 1e6),
          orientation: {
            alpha: event.alpha ?? 0,
            beta: event.beta ?? 0,
            gamma: event.gamma ?? 0,
          },
        });
        if (buffered.length > MAX_BUFFERED_SAMPLES) {
          buffered = buffered.slice(-MAX_BUFFERED_SAMPLES);
        }
      };

      host.addEventListener('deviceorientation', listener);
      running = true;
      return ok(undefined);
    },

    async drain(max: number) {
      if (!running) return err('FailedPrecondition', COMPONENT, 'sensor is not running');
      const taken = buffered.slice(0, Math.max(0, max));
      buffered = buffered.slice(taken.length);
      return ok(taken);
    },

    async stop() {
      if (listener) host.removeEventListener('deviceorientation', listener);
      listener = null;
      running = false;
      buffered = [];
      return ok(undefined);
    },
  };
}
