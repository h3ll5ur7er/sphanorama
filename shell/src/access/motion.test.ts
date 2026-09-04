// The motion adapter's job is to turn a hostile, inconsistent browser API into the contract the
// core declares. What is worth testing is exactly that translation — never that DeviceOrientation
// works, which would be testing the browser.
import { beforeEach, describe, expect, it, vi } from 'vitest';

import { createMotionSensorAccess } from './motion';
import { quaternionFromDeviceOrientation } from './orientation';

interface FakeWindow {
  DeviceOrientationEvent?: unknown;
  DeviceMotionEvent?: unknown;
  AbsoluteOrientationSensor?: unknown;
  screen?: unknown;
  addEventListener: ReturnType<typeof vi.fn>;
  removeEventListener: ReturnType<typeof vi.fn>;
}

function fakeWindow(overrides: Partial<FakeWindow> = {}): FakeWindow {
  return {
    DeviceOrientationEvent: class {},
    DeviceMotionEvent: class {},
    addEventListener: vi.fn(),
    removeEventListener: vi.fn(),
    ...overrides,
  } as FakeWindow;
}

/**
 * A stand-in for AbsoluteOrientationSensor with its readings under the test's control.
 *
 * The Generic Sensor API delivers by mutating the sensor and firing an event, which is unusual
 * enough that a fake shaped like the real thing is the only way to be sure the adapter reads it
 * at the right moment.
 */
function fakeSensor() {
  const listeners = new Map<string, () => void>();
  const sensor = {
    quaternion: null as number[] | null,
    timestamp: null as number | null,
    started: false,
    stopped: false,
    addEventListener(type: string, callback: () => void) { listeners.set(type, callback); },
    start() { this.started = true; },
    stop() { this.stopped = true; },
    emitReading(quaternion: number[], timestamp: number) {
      this.quaternion = quaternion;
      this.timestamp = timestamp;
      listeners.get('reading')?.();
    },
    emitError() { listeners.get('error')?.(); },
  };
  const constructed: { frequency?: number; referenceFrame?: string }[] = [];
  const ctor = function (this: unknown, options: { frequency?: number; referenceFrame?: string }) {
    constructed.push(options);
    return sensor;
  } as unknown as new (options: unknown) => typeof sensor;
  return { sensor, ctor, constructed };
}

/** The reading a level phone held in landscape produces: alpha 90, beta 0, gamma -90. */
const LANDSCAPE_LEVEL: [number, number, number] = [90, 0, -90];
const landscapeReading = [0.5, -0.5, 0.5, 0.5];

describe('capability detection', () => {
  it('reports OrientationOnly when only orientation is available', async () => {
    const access = createMotionSensorAccess(
      fakeWindow({ DeviceMotionEvent: undefined }) as never);
    await expect(access.capabilities()).resolves.toEqual({ ok: true, value: 'OrientationOnly' });
  });

  it('reports None rather than failing when the device has no motion sensors', async () => {
    // Absence is a supported outcome, not an error: the capture session switches PoseEngine to
    // vision-only and carries on (docs/03 UC-4).
    const access = createMotionSensorAccess(
      fakeWindow({ DeviceOrientationEvent: undefined, DeviceMotionEvent: undefined }) as never);
    await expect(access.capabilities()).resolves.toEqual({ ok: true, value: 'None' });
  });
});

describe('permission', () => {
  it('asks for permission where the platform requires it', async () => {
    // iOS gates motion behind an explicit grant that must follow a user gesture.
    const requestPermission = vi.fn().mockResolvedValue('granted');
    const win = fakeWindow({ DeviceOrientationEvent: { requestPermission } });
    const access = createMotionSensorAccess(win as never);
    await expect(access.start(60)).resolves.toEqual({ ok: true, value: undefined });
    expect(requestPermission).toHaveBeenCalled();
  });

  it('maps a declined grant onto SensorPermissionDenied', async () => {
    const requestPermission = vi.fn().mockResolvedValue('denied');
    const access = createMotionSensorAccess(
      fakeWindow({ DeviceOrientationEvent: { requestPermission } }) as never);
    const result = await access.start(60);
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.status.code).toBe('SensorPermissionDenied');
  });

  it('maps a rejected permission call onto SensorPermissionDenied too', async () => {
    // Safari rejects rather than resolving "denied" when the call is not user-initiated, and the
    // capture client has to react the same way to both.
    const requestPermission = vi.fn().mockRejectedValue(new Error('not user initiated'));
    const access = createMotionSensorAccess(
      fakeWindow({ DeviceOrientationEvent: { requestPermission } }) as never);
    const result = await access.start(60);
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.status.code).toBe('SensorPermissionDenied');
  });

  it('starts without asking on platforms that do not gate motion', async () => {
    const win = fakeWindow();
    const access = createMotionSensorAccess(win as never);
    await expect(access.start(60)).resolves.toEqual({ ok: true, value: undefined });
    expect(win.addEventListener).toHaveBeenCalledWith('deviceorientation', expect.any(Function));
  });

  it('refuses to start when there are no sensors to start', async () => {
    const access = createMotionSensorAccess(
      fakeWindow({ DeviceOrientationEvent: undefined, DeviceMotionEvent: undefined }) as never);
    const result = await access.start(60);
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.status.code).toBe('SensorUnavailable');
  });

  it('rejects a nonsense sample rate', async () => {
    const access = createMotionSensorAccess(fakeWindow() as never);
    const result = await access.start(0);
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.status.code).toBe('InvalidArgument');
  });
});

describe('sample delivery', () => {
  let win: FakeWindow;
  let listener: (event: unknown) => void;
  let access: ReturnType<typeof createMotionSensorAccess>;

  beforeEach(async () => {
    win = fakeWindow();
    access = createMotionSensorAccess(win as never);
    await access.start(60);
    const registration = win.addEventListener.mock.calls
      .find(([name]) => name === 'deviceorientation');
    listener = registration?.[1] as typeof listener;
  });

  it('buffers samples until drained', async () => {
    listener({ alpha: 10, beta: 0, gamma: 0, timeStamp: 1 });
    listener({ alpha: 20, beta: 0, gamma: 0, timeStamp: 2 });
    const drained = await access.drain(8);
    expect(drained.ok).toBe(true);
    if (drained.ok) expect(drained.value.length).toBe(2);
  });

  it('drain consumes rather than repeats', async () => {
    listener({ alpha: 10, beta: 0, gamma: 0, timeStamp: 1 });
    await access.drain(8);
    const second = await access.drain(8);
    if (second.ok) expect(second.value.length).toBe(0);
  });

  it('never returns more than the caller asked for', async () => {
    for (let i = 0; i < 10; i++) listener({ alpha: i, beta: 0, gamma: 0, timeStamp: i });
    const drained = await access.drain(3);
    if (drained.ok) expect(drained.value.length).toBe(3);
  });

  it('drops the oldest samples when the caller stops draining', async () => {
    // The capture loop can stall behind a slow frame. An unbounded buffer would grow until the
    // tab dies; stale orientation is worthless anyway, so the newest samples win.
    for (let i = 0; i < 5000; i++) listener({ alpha: i, beta: 0, gamma: 0, timeStamp: i });
    const drained = await access.drain(10_000);
    if (drained.ok) {
      expect(drained.value.length).toBeLessThan(5000);
      expect(drained.value.at(-1)!.orientation)
        .toEqual(quaternionFromDeviceOrientation(4999, 0, 0, 0));
    }
  });

  it('stops delivering after stop()', async () => {
    await access.stop();
    expect(win.removeEventListener).toHaveBeenCalledWith('deviceorientation', listener);
  });

  it('refuses to drain before start', async () => {
    const fresh = createMotionSensorAccess(fakeWindow() as never);
    const result = await fresh.drain(4);
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.status.code).toBe('FailedPrecondition');
  });
});

describe('events the platform could not fill in', () => {
  function listenerOf(host: ReturnType<typeof fakeWindow>) {
    return host.addEventListener.mock.calls[0][1] as (event: Event) => void;
  }

  it('drops an event whose angles the platform could not supply', async () => {
    // DeviceOrientationEvent's angles are nullable, and a browser that cannot produce an attitude
    // sends nulls rather than not firing. Coercing those to zero and marking the sample
    // hasOrientation hands PoseEngine a fabricated attitude — level, facing north — with the
    // confidence of a real measurement.
    const host = fakeWindow();
    const access = createMotionSensorAccess(host as never);
    await access.start(60);
    listenerOf(host)({ timeStamp: 1, alpha: null, beta: null, gamma: null } as unknown as Event);

    const drained = await access.drain(8);
    expect(drained.ok && drained.value).toEqual([]);
  });

  it('drops an event that is missing only one angle', async () => {
    const host = fakeWindow();
    const access = createMotionSensorAccess(host as never);
    await access.start(60);
    listenerOf(host)({ timeStamp: 1, alpha: 10, beta: null, gamma: 30 } as unknown as Event);
    const drained = await access.drain(8);
    expect(drained.ok && drained.value).toEqual([]);
  });

  it('keeps an event whose angles are all present, including zeros', async () => {
    // Zero is a real angle. Only null means "not available".
    const host = fakeWindow();
    const access = createMotionSensorAccess(host as never);
    await access.start(60);
    listenerOf(host)({ timeStamp: 1, alpha: 0, beta: 0, gamma: 0 } as unknown as Event);
    const drained = await access.drain(8);
    expect(drained.ok && drained.value).toHaveLength(1);
  });
});

describe('choosing a source', () => {
  it('prefers the quaternion sensor, so the primary pose is off the Euler singularity', () => {
    // Camera at the horizon means the screen plane contains gravity, which is beta near ±90 —
    // exactly where the Z-X'-Y'' triple stops having a unique decomposition and alpha and gamma
    // start trading places between samples. A quaternion has no such pose.
    const { ctor, constructed } = fakeSensor();
    const host = fakeWindow({ AbsoluteOrientationSensor: ctor });
    const access = createMotionSensorAccess(host as never);
    return access.start(60).then((started) => {
      expect(started.ok).toBe(true);
      expect(access.source()).toBe('AbsoluteOrientationSensor');
      expect(constructed[0]).toEqual({ frequency: 60, referenceFrame: 'device' });
      // And the event is not also subscribed: two sources at once would interleave two attitudes
      // of differing latency into one buffer.
      expect(host.addEventListener).not.toHaveBeenCalledWith(
        'deviceorientation', expect.any(Function));
    });
  });

  it('turns a sensor reading into a sample in the core frame', async () => {
    const { sensor, ctor } = fakeSensor();
    const access = createMotionSensorAccess(fakeWindow({ AbsoluteOrientationSensor: ctor }) as never);
    await access.start(60);
    sensor.emitReading(landscapeReading, 4);

    const drained = await access.drain(8);
    expect(drained.ok && drained.value).toHaveLength(1);
    if (drained.ok) {
      expect(drained.value[0].timestampNs).toBe(4_000_000);
      const expected = quaternionFromDeviceOrientation(...LANDSCAPE_LEVEL, 0);
      for (const key of ['w', 'x', 'y', 'z'] as const) {
        expect(drained.value[0].orientation[key]).toBeCloseTo(expected[key], 12);
      }
    }
  });

  it('falls back to the orientation event when the sensor fails to start', async () => {
    // The sensor needs permissions the page may not have and hardware the device may not carry,
    // and it reports both the same way: asynchronously, after start() has already returned.
    const { sensor, ctor } = fakeSensor();
    const host = fakeWindow({ AbsoluteOrientationSensor: ctor });
    const access = createMotionSensorAccess(host as never);
    await access.start(60);
    sensor.emitError();
    await Promise.resolve();

    expect(sensor.stopped).toBe(true);
    expect(access.source()).toBe('DeviceOrientation');
    expect(host.addEventListener).toHaveBeenCalledWith('deviceorientation', expect.any(Function));
  });

  it('falls back when the sensor constructor throws outright', async () => {
    // Constructing one is a SecurityError where the permissions policy forbids it, which is a
    // throw rather than an event, and happens before there is a sensor to listen to.
    const ctor = function () { throw new Error('SecurityError'); } as unknown as new () => unknown;
    const host = fakeWindow({ AbsoluteOrientationSensor: ctor });
    const access = createMotionSensorAccess(host as never);
    const started = await access.start(60);

    expect(started.ok).toBe(true);
    expect(access.source()).toBe('DeviceOrientation');
    expect(host.addEventListener).toHaveBeenCalledWith('deviceorientation', expect.any(Function));
  });

  it('falls back when the sensor refuses during start rather than afterwards', async () => {
    // Chromium reports a missing gyroscope by firing error from inside start(), before the call
    // returns — early enough that the adapter is still mid-way through installing the source it
    // is about to abandon.
    const { sensor, ctor } = fakeSensor();
    sensor.start = function () { this.started = true; this.emitError(); };
    const host = fakeWindow({ AbsoluteOrientationSensor: ctor });
    const access = createMotionSensorAccess(host as never);
    await access.start(60);
    await Promise.resolve();

    expect(access.source()).toBe('DeviceOrientation');
    expect(host.addEventListener).toHaveBeenCalledWith('deviceorientation', expect.any(Function));
  });

  it('reports nothing live when the sensor dies and there is no event to fall back to', async () => {
    // A device with the Generic Sensor API and no DeviceOrientationEvent — and a sensor that
    // errors. The fallback has nowhere to go, and a source left reading
    // AbsoluteOrientationSensor would have drain succeeding forever on a buffer nothing fills:
    // the capture loop reads no samples, the readout names a sensor that has been stopped, and
    // there is nothing on screen to say the phone stopped tracking.
    const { sensor, ctor } = fakeSensor();
    const host = fakeWindow({ AbsoluteOrientationSensor: ctor, DeviceOrientationEvent: undefined });
    const access = createMotionSensorAccess(host as never);
    await access.start(60);
    expect(access.source()).toBe('AbsoluteOrientationSensor');

    sensor.emitError();
    await Promise.resolve();

    expect(access.source()).toBe('none');
    const drained = await access.drain(8);
    expect(drained.ok).toBe(false);
    if (!drained.ok) expect(drained.status.code).toBe('FailedPrecondition');
  });

  it('uses the orientation event where there is no sensor at all', async () => {
    // iOS: no Generic Sensor API, and the event behind a permission gate.
    const access = createMotionSensorAccess(fakeWindow() as never);
    await access.start(60);
    expect(access.source()).toBe('DeviceOrientation');
  });

  it('reports no source before it is started, and none again after it is stopped', async () => {
    const { ctor } = fakeSensor();
    const access = createMotionSensorAccess(fakeWindow({ AbsoluteOrientationSensor: ctor }) as never);
    expect(access.source()).toBe('none');
    await access.start(60);
    await access.stop();
    expect(access.source()).toBe('none');
  });

  it('stops the sensor rather than leaving it draining the battery', async () => {
    const { sensor, ctor } = fakeSensor();
    const access = createMotionSensorAccess(fakeWindow({ AbsoluteOrientationSensor: ctor }) as never);
    await access.start(60);
    await access.stop();
    expect(sensor.stopped).toBe(true);
  });
});

describe('which way up the page is', () => {
  function screenAt(angle: number) {
    return { orientation: { angle } };
  }

  it('reads the screen angle at the moment of the sample, from either source', async () => {
    // Not once at start: the user turns the phone mid-session, and a cached angle would leave the
    // horizon a quarter turn out until the next capture.
    const screen = screenAt(0);
    const host = fakeWindow({ screen });
    const access = createMotionSensorAccess(host as never);
    await access.start(60);
    const listener = host.addEventListener.mock.calls
      .find((call) => call[0] === 'deviceorientation')![1] as (e: unknown) => void;

    listener({ alpha: 90, beta: 0, gamma: -90, timeStamp: 1 });
    screen.orientation.angle = 90;
    listener({ alpha: 90, beta: 0, gamma: -90, timeStamp: 2 });

    const drained = await access.drain(8);
    expect(drained.ok && drained.value).toHaveLength(2);
    if (!drained.ok) return;
    expect(drained.value[0].orientation)
      .toEqual(quaternionFromDeviceOrientation(...LANDSCAPE_LEVEL, 0));
    expect(drained.value[1].orientation)
      .toEqual(quaternionFromDeviceOrientation(...LANDSCAPE_LEVEL, 90));
  });

  it('treats a platform with no Screen Orientation API as unrotated', async () => {
    // Desktop Safari in a window, and every non-browser host the adapter is faked out in.
    const host = fakeWindow({ screen: undefined });
    const access = createMotionSensorAccess(host as never);
    await access.start(60);
    const listener = host.addEventListener.mock.calls
      .find((call) => call[0] === 'deviceorientation')![1] as (e: unknown) => void;
    listener({ alpha: 90, beta: 0, gamma: -90, timeStamp: 1 });

    const drained = await access.drain(8);
    if (drained.ok) {
      expect(drained.value[0].orientation)
        .toEqual(quaternionFromDeviceOrientation(...LANDSCAPE_LEVEL, 0));
    }
  });
});

describe('rates, where the platform measures them', () => {
  function listenerFor(host: ReturnType<typeof fakeWindow>, type: string) {
    const call = host.addEventListener.mock.calls.find((args) => args[0] === type);
    return call?.[1] as ((event: Event) => void) | undefined;
  }

  const spinning = { alpha: 90, beta: 0, gamma: 0 };

  it('attaches a measured rate to the samples that follow it', async () => {
    // Two event streams, one sample. The attitude decides when a sample exists and the most
    // recent rate rides along with it, because that is the shape PoseEngine fuses: a prediction
    // and a reading describing the same instant.
    const host = fakeWindow();
    const access = createMotionSensorAccess(host as never);
    await access.start(60);

    listenerFor(host, 'devicemotion')?.(
      { timeStamp: 1, rotationRate: spinning } as unknown as Event);
    listenerFor(host, 'deviceorientation')?.(
      { timeStamp: 2, alpha: 0, beta: 0, gamma: 0 } as unknown as Event);

    const drained = await access.drain(8);
    expect(drained.ok && drained.value).toHaveLength(1);
    const sample = (drained as { value: { angularVelocity?: unknown }[] }).value[0];
    // 90 deg/s about the device's Z, in radians, in the viewfinder's frame.
    expect(sample.angularVelocity).toBeDefined();
    expect((sample.angularVelocity as { z: number }).z).toBeCloseTo(Math.PI / 2, 6);
  });

  it('leaves a sample without rates when the platform fires an event with none', async () => {
    // Desktop Chrome fires devicemotion with a null rotationRate. Carrying a zeroed rate as
    // though it were measured is what makes a phone mid-swing read as perfectly still.
    const host = fakeWindow();
    const access = createMotionSensorAccess(host as never);
    await access.start(60);

    listenerFor(host, 'devicemotion')?.(
      { timeStamp: 1, rotationRate: null } as unknown as Event);
    listenerFor(host, 'deviceorientation')?.(
      { timeStamp: 2, alpha: 0, beta: 0, gamma: 0 } as unknown as Event);

    const drained = await access.drain(8);
    const sample = (drained as { value: { angularVelocity?: unknown }[] }).value[0];
    expect(sample.angularVelocity).toBeUndefined();
  });

  it('drops a rate whose axes the platform could not all supply', async () => {
    // Same rule the angles already follow: zero is a real rate and only null is unavailable, so a
    // partial reading is not completed with zeros.
    const host = fakeWindow();
    const access = createMotionSensorAccess(host as never);
    await access.start(60);

    listenerFor(host, 'devicemotion')?.(
      { timeStamp: 1, rotationRate: { alpha: 90, beta: null, gamma: 0 } } as unknown as Event);
    listenerFor(host, 'deviceorientation')?.(
      { timeStamp: 2, alpha: 0, beta: 0, gamma: 0 } as unknown as Event);

    const drained = await access.drain(8);
    const sample = (drained as { value: { angularVelocity?: unknown }[] }).value[0];
    expect(sample.angularVelocity).toBeUndefined();
  });

  it('does not attach a rate that has gone stale', async () => {
    // A rate from a second ago describes a motion that is over. Attaching it to a current
    // attitude would have the engine correcting against a prediction made from history, and the
    // staleness is invisible: both halves of the sample look equally like measurements.
    const host = fakeWindow();
    const access = createMotionSensorAccess(host as never);
    await access.start(60);

    listenerFor(host, 'devicemotion')?.(
      { timeStamp: 0, rotationRate: spinning } as unknown as Event);
    listenerFor(host, 'deviceorientation')?.(
      { timeStamp: 5_000, alpha: 0, beta: 0, gamma: 0 } as unknown as Event);

    const drained = await access.drain(8);
    const sample = (drained as { value: { angularVelocity?: unknown }[] }).value[0];
    expect(sample.angularVelocity).toBeUndefined();
  });

  it('does not attach a rate measured well after the attitude either', async () => {
    // The bound was one-sided, on the reasoning that a rate newer than the attitude is just the
    // two streams interleaving. True for the few milliseconds of skew between two DOM events, and
    // not a reason to accept any amount of it: a rate from five seconds in the future is
    // mis-associated for exactly the same reason one from five seconds ago is, and the fusion it
    // feeds cannot tell either way.
    const host = fakeWindow();
    const access = createMotionSensorAccess(host as never);
    await access.start(60);

    listenerFor(host, 'devicemotion')?.(
      { timeStamp: 5_000, rotationRate: spinning } as unknown as Event);
    listenerFor(host, 'deviceorientation')?.(
      { timeStamp: 0, alpha: 0, beta: 0, gamma: 0 } as unknown as Event);

    const drained = await access.drain(8);
    const sample = (drained as { value: { angularVelocity?: unknown }[] }).value[0];
    expect(sample.angularVelocity).toBeUndefined();
  });

  it('still attaches a rate a few milliseconds newer than the attitude', async () => {
    // The case the one-sided bound was protecting, and it survives: two DOM events on the same
    // clock arrive a few milliseconds apart in either order, and that is interleaving rather than
    // mis-association.
    const host = fakeWindow();
    const access = createMotionSensorAccess(host as never);
    await access.start(60);

    listenerFor(host, 'devicemotion')?.(
      { timeStamp: 8, rotationRate: spinning } as unknown as Event);
    listenerFor(host, 'deviceorientation')?.(
      { timeStamp: 0, alpha: 0, beta: 0, gamma: 0 } as unknown as Event);

    const drained = await access.drain(8);
    const sample = (drained as { value: { angularVelocity?: unknown }[] }).value[0];
    expect(sample.angularVelocity).toBeDefined();
  });

  it('stops listening for rates when the sensor stops', async () => {
    const host = fakeWindow();
    const access = createMotionSensorAccess(host as never);
    await access.start(60);
    await access.stop();
    expect(host.removeEventListener.mock.calls.map((args) => args[0]))
      .toContain('devicemotion');
  });

  it('reports GyroAccel where the platform can report rates', async () => {
    // Now that a sample says for itself whether its rate was measured, the capability can go back
    // to describing the platform. An over-claim costs nothing: the engine reads the sample.
    const access = createMotionSensorAccess(fakeWindow() as never);
    await expect(access.capabilities()).resolves.toEqual({ ok: true, value: 'GyroAccel' });
  });

  it('still asks for the motion grant iOS keeps separate from the orientation one', async () => {
    // Two gates, and granting one says nothing about the other. A denied motion grant is not a
    // failed start: the session runs on attitudes alone, which is what it did before rates
    // existed.
    const motionGate = vi.fn().mockResolvedValue('denied');
    const host = fakeWindow({
      DeviceOrientationEvent: class { static requestPermission = vi.fn().mockResolvedValue('granted'); },
      DeviceMotionEvent: class { static requestPermission = motionGate; },
    });
    const access = createMotionSensorAccess(host as never);
    const started = await access.start(60);
    expect(started.ok).toBe(true);
    expect(motionGate).toHaveBeenCalled();
  });
});
