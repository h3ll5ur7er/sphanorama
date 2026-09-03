// What matters here is the mapping from getUserMedia's failure vocabulary onto StatusCodes the
// core can branch on. A user who declined the camera and a device that has none need different
// words on screen, and by the time it reaches the capture client the difference has to survive.
import { describe, expect, it, vi } from 'vitest';

import { createCameraAccess } from './camera';

function fakeMedia(behaviour: {
  stream?: unknown;
  error?: { name: string; message?: string };
}) {
  return {
    getUserMedia: vi.fn(async () => {
      if (behaviour.error) {
        const error = new Error(behaviour.error.message ?? 'failed');
        error.name = behaviour.error.name;
        throw error;
      }
      return behaviour.stream ?? {
        getVideoTracks: () => [{
          getSettings: () => ({ width: 1920, height: 1080 }),
          getCapabilities: () => ({ torch: true }),
          stop: vi.fn(),
        }],
        getTracks: () => [{ stop: vi.fn() }],
      };
    }),
  };
}

describe('opening the camera', () => {
  it('reports the resolution the track actually settled on', async () => {
    // Requested and granted resolution differ constantly across devices; the coverage planner
    // sizes cells from what we got, not what we asked for.
    const camera = createCameraAccess(fakeMedia({}) as never);
    const result = await camera.open({ preferRearCamera: true });
    expect(result.ok).toBe(true);
    if (result.ok) {
      expect(result.value.maxWidth).toBe(1920);
      expect(result.value.maxHeight).toBe(1080);
    }
  });

  it('asks for the rear camera when told to', async () => {
    const media = fakeMedia({});
    const camera = createCameraAccess(media as never);
    await camera.open({ preferRearCamera: true });
    const calls = media.getUserMedia.mock.calls as unknown as Array<[{ video: { facingMode: unknown } }]>;
    expect(calls[0]?.[0].video.facingMode).toEqual({ ideal: 'environment' });
  });

  it('maps a declined camera onto a permission failure', async () => {
    const camera = createCameraAccess(fakeMedia({ error: { name: 'NotAllowedError' } }) as never);
    const result = await camera.open({ preferRearCamera: true });
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.status.code).toBe('SensorPermissionDenied');
  });

  it('distinguishes no camera at all from a declined one', async () => {
    const camera = createCameraAccess(fakeMedia({ error: { name: 'NotFoundError' } }) as never);
    const result = await camera.open({ preferRearCamera: true });
    expect(result.ok).toBe(false);
    if (!result.ok) expect(result.status.code).toBe('CameraUnavailable');
  });

  it('maps a camera held by another app onto CameraUnavailable', async () => {
    const camera = createCameraAccess(fakeMedia({ error: { name: 'NotReadableError' } }) as never);
    const result = await camera.open({ preferRearCamera: true });
    if (!result.ok) expect(result.status.code).toBe('CameraUnavailable');
  });

  it('keeps the underlying error text for diagnosis', async () => {
    const camera = createCameraAccess(
      fakeMedia({ error: { name: 'NotAllowedError', message: 'user gesture required' } }) as never);
    const result = await camera.open({ preferRearCamera: true });
    if (!result.ok) expect(result.status.detail).toContain('user gesture required');
  });

  it('reports Unsupported where there is no media API at all', async () => {
    // Insecure origins have no navigator.mediaDevices, which is a deployment mistake rather than
    // a device limitation and should read differently in the UI.
    const camera = createCameraAccess(undefined);
    const result = await camera.open({ preferRearCamera: true });
    if (!result.ok) expect(result.status.code).toBe('Unsupported');
  });
});

describe('lifecycle', () => {
  it('refuses to hand out a stream before open', () => {
    const camera = createCameraAccess(fakeMedia({}) as never);
    expect(camera.stream()).toBeNull();
  });

  it('exposes the stream once open, for the viewfinder to render', async () => {
    const camera = createCameraAccess(fakeMedia({}) as never);
    await camera.open({ preferRearCamera: true });
    expect(camera.stream()).not.toBeNull();
  });

  it('stops every track on close so the camera light goes out', async () => {
    const stop = vi.fn();
    const stream = {
      getVideoTracks: () => [{ getSettings: () => ({ width: 640, height: 480 }), getCapabilities: () => ({}), stop }],
      getTracks: () => [{ stop }, { stop }],
    };
    const camera = createCameraAccess(fakeMedia({ stream }) as never);
    await camera.open({ preferRearCamera: true });
    await camera.close();
    expect(stop).toHaveBeenCalledTimes(2);
    expect(camera.stream()).toBeNull();
  });

  it('closing an unopened camera is harmless', async () => {
    const camera = createCameraAccess(fakeMedia({}) as never);
    await expect(camera.close()).resolves.toEqual({ ok: true, value: undefined });
  });
});

/**
 * A track that records the constraints it was asked for and reports back whatever modes the test
 * says it settled on — which is the distinction that matters, since asking is not applying.
 */
function fakeTrack(options: {
  capabilities?: Record<string, unknown>;
  /** What getSettings() reports after applyConstraints resolves. Defaults to what was asked. */
  settleAs?: Record<string, string>;
  rejectWith?: string;
} = {}) {
  const applied: unknown[] = [];
  let settings: Record<string, unknown> = { width: 1920, height: 1080 };
  const track = {
    applied,
    getSettings: () => settings,
    getCapabilities: () => options.capabilities ?? {
      exposureMode: ['continuous', 'manual'],
      whiteBalanceMode: ['continuous', 'manual'],
      focusMode: ['continuous', 'manual'],
    },
    async applyConstraints(constraints: unknown) {
      applied.push(constraints);
      if (options.rejectWith) {
        const error = new Error('constraint refused');
        error.name = options.rejectWith;
        throw error;
      }
      const asked = (constraints as { advanced?: Record<string, string>[] }).advanced?.[0] ?? {};
      settings = { ...settings, ...(options.settleAs ?? asked) };
    },
    stop: vi.fn(),
  };
  return track;
}

function mediaWith(track: ReturnType<typeof fakeTrack>) {
  return {
    getUserMedia: vi.fn(async () => ({
      getVideoTracks: () => [track],
      getTracks: () => [track],
    })),
  };
}

describe('reporting which locks the camera has', () => {
  it('reports a lock as supported only when the track offers a manual mode', async () => {
    const camera = createCameraAccess(mediaWith(fakeTrack()) as never);
    const opened = await camera.open({ preferRearCamera: true });

    expect(opened.ok).toBe(true);
    if (opened.ok) {
      expect(opened.value.supportsExposureLock).toBe(true);
      expect(opened.value.supportsFocusLock).toBe(true);
      expect(opened.value.supportsWhiteBalanceLock).toBe(true);
    }
  });

  it('reports a lock as unsupported when the track only ever does it automatically', async () => {
    // A desktop webcam, typically. Claiming the lock here is how a burst ends up compared on
    // brightness while every count-based check still passes.
    const track = fakeTrack({ capabilities: { exposureMode: ['continuous'] } });
    const camera = createCameraAccess(mediaWith(track) as never);
    const opened = await camera.open({ preferRearCamera: true });

    expect(opened.ok).toBe(true);
    if (opened.ok) {
      expect(opened.value.supportsExposureLock).toBe(false);
      expect(opened.value.supportsFocusLock).toBe(false);
    }
  });

  it('reports no locks when the track will not say what it can do', async () => {
    const track = fakeTrack({ capabilities: {} });
    const camera = createCameraAccess(mediaWith(track) as never);
    const opened = await camera.open({ preferRearCamera: true });

    if (opened.ok) expect(opened.value.supportsExposureLock).toBe(false);
  });
});

describe('applying the locks', () => {
  it('asks the track for manual modes and confirms they took', async () => {
    const track = fakeTrack();
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });

    const locked = await camera.setLocks({ exposure: true, whiteBalance: true, focus: true });

    expect(locked.ok).toBe(true);
    if (locked.ok) expect(locked.value).toEqual({ exposure: true, whiteBalance: true, focus: true });
    expect(track.applied).toHaveLength(1);
  });

  it('reports a lock as not held when the track quietly stayed automatic', async () => {
    // applyConstraints resolving is not the same as the constraint being honoured, and this is
    // the case that makes the difference matter: a burst told its exposure is fixed, comparing
    // candidates on sharpness, while the camera keeps metering between frames.
    const track = fakeTrack({ settleAs: { exposureMode: 'continuous', focusMode: 'manual' } });
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });

    const locked = await camera.setLocks({ exposure: true, whiteBalance: false, focus: true });

    expect(locked.ok).toBe(true);
    if (locked.ok) {
      expect(locked.value.exposure).toBe(false);
      expect(locked.value.focus).toBe(true);
    }
  });

  it('reports nothing locked when the track refuses the constraints outright', async () => {
    const track = fakeTrack({ rejectWith: 'OverconstrainedError' });
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });

    const locked = await camera.setLocks({ exposure: true, whiteBalance: true, focus: true });

    expect(locked.ok).toBe(true);
    if (locked.ok) {
      expect(locked.value).toEqual({ exposure: false, whiteBalance: false, focus: false });
    }
  });

  it('releases by asking for continuous again', async () => {
    const track = fakeTrack();
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });
    await camera.setLocks({ exposure: true, whiteBalance: true, focus: true });

    const released = await camera.setLocks({ exposure: false, whiteBalance: false, focus: false });

    expect(released.ok).toBe(true);
    if (released.ok) expect(released.value.exposure).toBe(false);
    expect(track.applied).toHaveLength(2);
  });

  it('refuses when no camera is open, rather than reporting locks it cannot have', async () => {
    const camera = createCameraAccess(mediaWith(fakeTrack()) as never);
    const locked = await camera.setLocks({ exposure: true, whiteBalance: true, focus: true });
    expect(locked.ok).toBe(false);
  });
});
