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
