// The motion adapter's job is to turn a hostile, inconsistent browser API into the contract the
// core declares. What is worth testing is exactly that translation — never that DeviceOrientation
// works, which would be testing the browser.
import { beforeEach, describe, expect, it, vi } from 'vitest';

import { createMotionSensorAccess } from './motion';

interface FakeWindow {
  DeviceOrientationEvent?: unknown;
  DeviceMotionEvent?: unknown;
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

describe('capability detection', () => {
  it('reports GyroAccel when the motion API exists', async () => {
    const access = createMotionSensorAccess(fakeWindow() as never);
    await expect(access.capabilities()).resolves.toEqual({ ok: true, value: 'GyroAccel' });
  });

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
      expect(drained.value.at(-1)!.orientation.alpha).toBe(4999);
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
