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

  it('requires the rear camera rather than merely preferring it', async () => {
    // `ideal` is scored, not obeyed: getUserMedia picks the device with the lowest *combined*
    // fitness distance over every ideal constraint, so a front camera that matches the requested
    // resolution more closely outscores a rear one that does not, and the app quietly shoots a
    // photo sphere as a selfie. Measured on a Pixel 9 Pro XL. Which way the phone is facing is
    // not a preference to be traded against pixels.
    const media = fakeMedia({});
    const camera = createCameraAccess(media as never);
    await camera.open({ preferRearCamera: true, preferredWidth: 1280, preferredHeight: 960 });
    const calls = media.getUserMedia.mock.calls as unknown as Array<[{ video: { facingMode: unknown } }]>;
    expect(calls[0]?.[0].video.facingMode).toEqual({ exact: 'environment' });
  });

  it('falls back to a preference when the device has no camera facing that way', async () => {
    // `exact` is a filter, not a score: a laptop with only a front camera answers
    // OverconstrainedError and would otherwise get no camera at all rather than the one it has.
    let attempt = 0;
    const media = {
      getUserMedia: vi.fn(async () => {
        attempt += 1;
        if (attempt === 1) {
          const error = new Error('facingMode');
          error.name = 'OverconstrainedError';
          throw error;
        }
        return {
          getVideoTracks: () => [{
            getSettings: () => ({ width: 1280, height: 720 }),
            getCapabilities: () => ({}),
            stop: vi.fn(),
          }],
          getTracks: () => [{ stop: vi.fn() }],
        };
      }),
    };
    const camera = createCameraAccess(media as never);
    const result = await camera.open({ preferRearCamera: true });
    expect(result.ok).toBe(true);
    const calls = media.getUserMedia.mock.calls as unknown as Array<[{ video: { facingMode: unknown } }]>;
    expect(calls[0]?.[0].video.facingMode).toEqual({ exact: 'environment' });
    expect(calls[1]?.[0].video.facingMode).toEqual({ ideal: 'environment' });
  });

  it('asks for a frame at least as large as the one that will be stored', async () => {
    // Left unasked, getUserMedia hands back the browser's default — 640x480 in Chromium, which is
    // half the long edge the grabber keeps and a quarter of its pixels. Every frame the core has
    // ever scored or stored has been that default upscaled by nobody: the cap was guarding
    // nothing. The adapter's job is only to pass the ask through; which number to ask for is the
    // client's.
    const media = fakeMedia({});
    const camera = createCameraAccess(media as never);
    await camera.open({ preferRearCamera: true, preferredWidth: 1280 });
    const video = (media.getUserMedia.mock.calls as unknown as Array<[{ video: Record<string, unknown> }]>)[0]?.[0].video;
    expect(video?.width).toEqual({ ideal: 1280 });
  });

  it('imposes no shape of its own when the client asks only for a size', async () => {
    // The adapter states what it was told and nothing more. Which shape to ask for is the
    // client's call — main.ts asks for 4:3 because that is the sensor's own frame and 16:9 is a
    // crop of it — and an adapter that invented a height would quietly overrule that.
    const media = fakeMedia({});
    const camera = createCameraAccess(media as never);
    await camera.open({ preferRearCamera: true, preferredWidth: 1280 });
    const video = (media.getUserMedia.mock.calls as unknown as Array<[{ video: Record<string, unknown> }]>)[0]?.[0].video;
    expect(video?.height).toBeUndefined();
  });

  it('passes a stated height through as an ideal too', async () => {
    const media = fakeMedia({});
    const camera = createCameraAccess(media as never);
    await camera.open({ preferRearCamera: true, preferredWidth: 1280, preferredHeight: 960 });
    const video = (media.getUserMedia.mock.calls as unknown as Array<[{ video: Record<string, unknown> }]>)[0]?.[0].video;
    expect(video?.height).toEqual({ ideal: 960 });
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
  /**
   * Whether this camera can satisfy one advanced constraint set. The spec applies such a set only
   * if the whole of it can be satisfied, and a set it cannot is skipped rather than an error — so
   * a camera that advertises a mode and then will not take it is silence, not a rejection.
   */
  refuses?: (set: Record<string, unknown>) => boolean;
  /** Settings the camera reports before anything is asked of it. */
  initial?: Record<string, unknown>;
  /** A track with no `getCapabilities` at all — Safari has none. */
  omitCapabilitiesApi?: boolean;
  /** A `getCapabilities` that throws instead of answering. */
  capabilitiesThrow?: string;
} = {}) {
  const applied: unknown[] = [];
  let settings: Record<string, unknown> = { width: 1920, height: 1080, ...options.initial };
  const track = {
    applied,
    getSettings: () => settings,
    getCapabilities: () => {
      if (options.capabilitiesThrow) {
        const error = new Error('no capabilities for you');
        error.name = options.capabilitiesThrow;
        throw error;
      }
      return options.capabilities ?? {
        exposureMode: ['continuous', 'manual'],
        whiteBalanceMode: ['continuous', 'manual'],
        focusMode: ['continuous', 'manual'],
      };
    },
    async applyConstraints(constraints: unknown) {
      applied.push(constraints);
      if (options.rejectWith) {
        const error = new Error('constraint refused');
        error.name = options.rejectWith;
        throw error;
      }
      for (const asked of (constraints as { advanced?: Record<string, unknown>[] }).advanced ?? []) {
        if (options.refuses?.(asked)) continue;
        settings = { ...settings, ...(options.settleAs ?? asked) };
      }
    },
    stop: vi.fn(),
  };
  // Deleted rather than left undefined, because the adapter has to survive the property simply
  // not being there — which is the shape a browser without the API actually has.
  if (options.omitCapabilitiesApi) delete (track as { getCapabilities?: unknown }).getCapabilities;
  return track;
}

/** A camera that hands out a different track each time it is opened, the way switching does. */
function mediaHanding(tracks: ReturnType<typeof fakeTrack>[]) {
  let next = 0;
  return {
    getUserMedia: vi.fn(async () => {
      const track = tracks[Math.min(next++, tracks.length - 1)];
      return { getVideoTracks: () => [track], getTracks: () => [track] };
    }),
  };
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

describe('what the camera says it offers', () => {
  // Read once when the camera opens and reported, never used to decide what to ask for: browsers
  // under-report, so a negotiation gated on this list would give a browser that lists nothing no
  // locks at all. The two tests at the end of the next block are what hold that line.
  it('reports the modes the track lists, control by control', async () => {
    // Three different lists on purpose. With one list repeated, an adapter that read
    // `focusMode` into all three would pass.
    const track = fakeTrack({ capabilities: {
      exposureMode: ['continuous', 'manual'],
      whiteBalanceMode: ['continuous'],
      focusMode: ['manual', 'single-shot'],
    } });
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });

    expect(camera.offeredModes()).toEqual({
      exposure: ['continuous', 'manual'],
      whiteBalance: ['continuous'],
      focus: ['manual', 'single-shot'],
    });
  });

  it('separates a camera that offers only continuous from a browser that said nothing', async () => {
    // The whole point of asking. "Only continuous" is the camera answering; a missing key is the
    // browser declining to, and a row that renders them the same way is how a refusal ends up
    // meaning nothing — which is what sent us guessing at a Pixel twice.
    const track = fakeTrack({ capabilities: { exposureMode: ['continuous'] } });
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });

    expect(camera.offeredModes().exposure).toEqual(['continuous']);
    expect(camera.offeredModes().focus).toBeNull();
  });

  it('reports nothing, and still opens, where there is no getCapabilities at all', async () => {
    // Safari. The camera works; the question about it is what cannot be put.
    const track = fakeTrack({ omitCapabilitiesApi: true });
    const camera = createCameraAccess(mediaWith(track) as never);
    const opened = await camera.open({ preferRearCamera: true });

    expect(opened.ok).toBe(true);
    expect(camera.offeredModes())
      .toEqual({ exposure: null, whiteBalance: null, focus: null });
  });

  it('survives a getCapabilities that throws, rather than failing the open', async () => {
    // A working camera reported as unavailable is the worst of the three outcomes: the app is
    // dead for a device whose only fault is that it would not answer a question asked for a
    // status line.
    const track = fakeTrack({ capabilitiesThrow: 'InvalidStateError' });
    const camera = createCameraAccess(mediaWith(track) as never);
    const opened = await camera.open({ preferRearCamera: true });

    expect(opened.ok).toBe(true);
    if (opened.ok) expect(opened.value.maxWidth).toBe(1920);
    expect(camera.offeredModes())
      .toEqual({ exposure: null, whiteBalance: null, focus: null });
  });

  it('treats a key that is not a list of modes as nothing reported', async () => {
    // `getCapabilities` is a dictionary the browser fills in as it likes, and a key present with
    // something else in it says no more about the camera than an absent one does.
    const track = fakeTrack({ capabilities: { exposureMode: 'manual' } });
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });

    expect(camera.offeredModes().exposure).toBeNull();
  });

  it('says nothing before a camera is open, and again once it closes', async () => {
    // A list belongs to a track. Left standing after close it would describe a camera that is no
    // longer running, on the next row rendered.
    const camera = createCameraAccess(mediaWith(fakeTrack()) as never);
    expect(camera.offeredModes().exposure).toBeNull();

    await camera.open({ preferRearCamera: true });
    expect(camera.offeredModes().exposure).toEqual(['continuous', 'manual']);

    await camera.close();
    expect(camera.offeredModes().exposure).toBeNull();
  });

  it('hands out a report a caller cannot write to', async () => {
    // The lists are `readonly string[]` to TypeScript and were an ordinary object at runtime, so
    // the accessor handed a caller the adapter's own state to do as it liked with. Frozen rather
    // than copied: the row is rendered on the capture tick, so a defensive copy would be garbage
    // per frame to defend against something that happens per camera.
    const camera = createCameraAccess(mediaWith(fakeTrack()) as never);
    await camera.open({ preferRearCamera: true });

    const modes = camera.offeredModes();
    // Modules are strict mode, so a write to a frozen object throws rather than being ignored —
    // which is the behaviour worth having: a caller doing this has a bug and should hear about it.
    expect(() => { (modes as { exposure: unknown }).exposure = ['nonsense']; }).toThrow();
    expect(() => { (modes.exposure as string[]).push('nonsense'); }).toThrow();

    expect(camera.offeredModes().exposure).toEqual(['continuous', 'manual']);
  });

  it('does not let a caller poison the silence the next camera starts from', async () => {
    // The sharp end of it, and the reason freezing the lists alone is not enough. Before a camera
    // opens and after one closes the accessor hands back a single module-level value shared by
    // every camera this page will ever open — so one write to it is not a corrupted row, it is
    // every row after it, for the life of the tab.
    const camera = createCameraAccess(mediaWith(fakeTrack({ capabilities: {} })) as never);

    const silence = camera.offeredModes();
    expect(() => { (silence as { exposure: unknown }).exposure = ['manual']; }).toThrow();

    await camera.open({ preferRearCamera: true });
    expect(camera.offeredModes().exposure).toBeNull();
  });

  it('replaces the list when another camera is opened', async () => {
    // Same reason a refusal is not carried across an open: what a camera offers is a fact about
    // that camera.
    const first = fakeTrack();
    const second = fakeTrack({ capabilities: { exposureMode: ['continuous'] } });
    const camera = createCameraAccess(mediaHanding([first, second]) as never);

    await camera.open({ preferRearCamera: true });
    await camera.open({ preferRearCamera: true });

    expect(camera.offeredModes().exposure).toEqual(['continuous']);
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
    // What it asked for, rather than how many times: each lock is negotiated on its own now, so a
    // count would pin the number of round trips instead of the thing under test.
    expect(JSON.stringify(track.applied)).toContain('"exposureMode":"manual"');
    expect(JSON.stringify(track.applied)).toContain('"focusMode":"manual"');
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
    expect(JSON.stringify(track.applied.slice(-3))).toContain('"exposureMode":"continuous"');
  });

  it('does not let one refused lock take the others down with it', async () => {
    // The reading from a Pixel: `focus · exposure refused · white balance refused`. An advanced
    // constraint set is applied only if the *whole* of it can be satisfied, so asking for all
    // three at once means one mode the camera will not take discards the two it would have.
    const track = fakeTrack({
      refuses: (set) => 'exposureMode' in set && set.exposureMode !== 'continuous',
    });
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });

    const locked = await camera.setLocks({ exposure: true, whiteBalance: true, focus: true });

    expect(locked.ok).toBe(true);
    if (locked.ok) {
      expect(locked.value.exposure).toBe(false);
      expect(locked.value.focus).toBe(true);
      expect(locked.value.whiteBalance).toBe(true);
    }
  });

  it('offers the exposure time it is already using when it asks for manual', async () => {
    // `manual` on Android generally means "I will tell you the number", and a camera asked to go
    // manual without one refuses. The number it is metering at right now is the one that holds
    // the exposure where the burst wants it: exactly where it was when the cell was framed.
    const track = fakeTrack({
      initial: { exposureTime: 312 },
      refuses: (set) => set.exposureMode === 'manual' && set.exposureTime === undefined,
    });
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });

    const locked = await camera.setLocks({ exposure: true, whiteBalance: false, focus: false });

    expect(locked.ok).toBe(true);
    if (locked.ok) expect(locked.value.exposure).toBe(true);
    expect(JSON.stringify(track.applied)).toContain('"exposureTime":312');
  });

  it('falls back to single-shot for a camera that will not go manual', async () => {
    // The other way to say "stop metering": one-and-done rather than a number. A camera that
    // takes neither is a camera with no lock, but one that takes only this is common enough that
    // giving up after `manual` would leave a burst metering for no reason.
    const track = fakeTrack({
      capabilities: { exposureMode: ['continuous', 'manual', 'single-shot'] },
      refuses: (set) => set.exposureMode === 'manual',
    });
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });

    const locked = await camera.setLocks({ exposure: true, whiteBalance: false, focus: false });

    expect(locked.ok).toBe(true);
    if (locked.ok) expect(locked.value.exposure).toBe(true);
  });

  it('reads single-shot back as a lock, because it is one', async () => {
    const track = fakeTrack({ settleAs: { exposureMode: 'single-shot' } });
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });

    const locked = await camera.setLocks({ exposure: true, whiteBalance: false, focus: false });
    expect(locked.ok).toBe(true);
    if (locked.ok) expect(locked.value.exposure).toBe(true);
  });

  it('stops asking for a lock this camera has already refused', async () => {
    // Locks are applied before every burst, and a sphere is twenty-eight of them. A camera that
    // said no once will say no every time, and each attempt is a round trip in front of the
    // frames — the delay lands between framing a cell and capturing it.
    const track = fakeTrack({ refuses: (set) => 'exposureMode' in set && set.exposureMode !== 'continuous' });
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });

    await camera.setLocks({ exposure: true, whiteBalance: false, focus: false });
    const afterFirst = track.applied.length;
    await camera.setLocks({ exposure: true, whiteBalance: false, focus: false });

    // Not asked at all on the second pass, rather than asked fewer times. A count would go on
    // passing while one of the two attempts survived, which is most of the delay and all of the
    // pointlessness.
    const second = JSON.stringify(track.applied.slice(afterFirst));
    expect(second).not.toContain('exposureMode');
    // And the release of what was not wanted still happens, so this is not passing because the
    // second call did nothing whatsoever.
    expect(second).toContain('"focusMode":"continuous"');
  });

  it('lets go of the camera it was holding when it opens another', async () => {
    // Opening twice is switching cameras, or re-enabling after a stop. The stream that was open
    // has to go: nothing else is holding it, so it would run for the life of the page with the
    // indicator lit — which a user reads, correctly, as the app watching them.
    const first = fakeTrack();
    const second = fakeTrack();
    const camera = createCameraAccess(mediaHanding([first, second]) as never);

    await camera.open({ preferRearCamera: true });
    await camera.open({ preferRearCamera: true });

    expect(first.stop).toHaveBeenCalled();
  });

  it('does not hold a new camera to what the last one refused', async () => {
    // What a camera will not do is a fact about that camera. Carrying a refusal across an open
    // is how the second camera silently loses a lock it would have given — and the comment on
    // that memory said it was cleared with the track, which is the claim under test.
    const stubborn = fakeTrack({
      refuses: (set) => 'exposureMode' in set && set.exposureMode !== 'continuous',
    });
    const willing = fakeTrack();
    const camera = createCameraAccess(mediaHanding([stubborn, willing]) as never);

    await camera.open({ preferRearCamera: true });
    await camera.setLocks({ exposure: true, whiteBalance: false, focus: false });
    await camera.open({ preferRearCamera: true });
    const locked = await camera.setLocks({ exposure: true, whiteBalance: false, focus: false });

    expect(locked.ok).toBe(true);
    if (locked.ok) expect(locked.value.exposure).toBe(true);
  });

  it('asks for a mode the capabilities never advertised', async () => {
    // Capabilities are an observation, not a gate. Browsers under-report: a device may take a
    // constraint it never listed, and this camera does exactly that. Skipping what was not
    // advertised would cost this lock, and would cost every lock on a browser that lists nothing.
    const track = fakeTrack({ capabilities: { exposureMode: ['continuous'] } });
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });

    const locked = await camera.setLocks({ exposure: true, whiteBalance: false, focus: false });

    expect(locked.ok).toBe(true);
    if (locked.ok) expect(locked.value.exposure).toBe(true);
    expect(JSON.stringify(track.applied)).toContain('"exposureMode":"manual"');
  });

  it('negotiates unchanged for a camera that reports no capabilities at all', async () => {
    // The iPhone: no lists to read, and a white balance lock that holds anyway. Asserted as the
    // whole sequence rather than an outcome, because the regression to guard against is one that
    // quietly asks for *less* — and a camera that gives its lock on the fallback still gives it
    // whether or not the first ask happened.
    const track = fakeTrack({
      omitCapabilitiesApi: true,
      refuses: (set) => set.whiteBalanceMode === 'manual',
    });
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });

    const locked = await camera.setLocks({ exposure: false, whiteBalance: true, focus: false });

    expect(locked.ok).toBe(true);
    if (locked.ok) {
      expect(locked.value).toEqual({ exposure: false, whiteBalance: true, focus: false });
    }
    expect(track.applied).toEqual([
      { advanced: [{ exposureMode: 'continuous' }] },
      { advanced: [{ whiteBalanceMode: 'manual' }] },
      { advanced: [{ whiteBalanceMode: 'single-shot' }] },
      { advanced: [{ focusMode: 'continuous' }] },
    ]);
  });

  it('refuses when no camera is open, rather than reporting locks it cannot have', async () => {
    const camera = createCameraAccess(mediaWith(fakeTrack()) as never);
    const locked = await camera.setLocks({ exposure: true, whiteBalance: true, focus: true });
    expect(locked.ok).toBe(false);
  });
});
