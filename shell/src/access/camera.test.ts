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

  it('reports the track frame rate, so the core has a camera-rate floor to apply', async () => {
    // `CameraCapabilities.maxBurstFps` is what `CaptureSessionManager` floors a burst's interval
    // and settle with (ADR 0018, ADR 0032): `PeekPreviewFrame` borrows the *latest* preview frame,
    // so asking for frames faster than the camera makes them fills a burst with duplicates of one
    // exposure, and selection then ranks a frame against copies of itself.
    //
    // This adapter never set the field. The whole floor was therefore dead in the only client
    // there is — zero means "the platform will not say", which is the one answer that turns it
    // off — while the core, the ADRs and the contract all described it as working. Found by a
    // reviewer reading the core's arithmetic and asking who supplies the number.
    const camera = createCameraAccess(fakeMedia({
      stream: {
        getVideoTracks: () => [{
          getSettings: () => ({ width: 1280, height: 720, frameRate: 30 }),
          getCapabilities: () => ({}),
          stop: vi.fn(),
        }],
        getTracks: () => [{ stop: vi.fn() }],
      },
    }) as never);
    const result = await camera.open({ preferRearCamera: true });
    expect(result.ok).toBe(true);
    if (result.ok) expect(result.value.maxBurstFps).toBe(30);
  });

  it('reports the track as it is now, not as it was when it opened', async () => {
    // ADR 0045's push half. `ICameraAccess::Capabilities()` lets the core re-ask at arm time, but
    // the port it asks lives in the worker and answers from what this page last pushed — so a
    // pull with nothing pushing behind it reads a cache and returns what `open()` said. Three
    // reviewers found that independently.
    //
    // What makes it matter is that this app changes the thing it depends on: `setLocks` drives
    // `applyConstraints({ exposureMode: 'manual' })`, and a camera whose exposure has just been
    // pinned long is exactly the one that drops from 30 fps to 15.
    const settings = { width: 1280, height: 720, frameRate: 30 };
    const camera = createCameraAccess(fakeMedia({
      stream: {
        getVideoTracks: () => [{
          getSettings: () => settings,
          getCapabilities: () => ({}),
          stop: vi.fn(),
        }],
        getTracks: () => [{ stop: vi.fn() }],
      },
    }) as never);
    const opened = await camera.open({ preferRearCamera: true });
    expect(opened.ok && opened.value.maxBurstFps).toBe(30);

    // The track slows, as one does under a long exposure.
    settings.frameRate = 15;

    const now = camera.capabilities();
    expect(now.maxBurstFps, 'the adapter answered from what open() saw, not from the track')
      .toBe(15);
    // And the rest of the answer is the same shape as `open`'s, so the two cannot disagree about
    // anything but what actually moved.
    expect(now.maxWidth).toBe(1280);
    expect(now.maxHeight).toBe(720);
  });

  it('says nothing about a camera it is not holding', async () => {
    // No track, no answer to give. Zeros rather than a stale last-known set: the core reads 0 as
    // "the platform will not say", which is true of a camera that is gone.
    //
    // Asked *after* a camera has been open and reported real numbers, which is the half a reviewer
    // showed was missing: asked before any open, this assertion is satisfied by a default, and an
    // implementation that remembered the last camera for ever would pass it. Zeros here are only
    // evidence of anything if there was something else to answer with.
    const camera = createCameraAccess(fakeMedia({}) as never);
    expect(camera.capabilities().maxWidth, 'nothing open, so nothing to say').toBe(0);

    const opened = await camera.open({ preferRearCamera: true });
    expect(opened.ok && opened.value.maxWidth).toBe(1920);

    await camera.close();
    const after = camera.capabilities();
    expect(after.maxWidth, 'the adapter kept answering for a camera it had closed').toBe(0);
    expect(after.maxBurstFps).toBe(0);
    expect(after.supportsTorch, 'and kept a capability of it too').toBe(false);
  });

  it('says nothing about a track that has ended, rather than half of it', async () => {
    // The camera the page actually loses. `close()` is the only thing that clears `active` and the
    // page never calls it — the core's route out is `closeCamera`, which stops the tracks — and
    // `stop()` does not remove a track from its stream. So the adapter goes on being handed a dead
    // track, and a dead track answers *unevenly*: `getSettings()` has dropped the geometry and
    // `getCapabilities()` still lists every mode the device supports.
    //
    // Read without a guard that is a mixture — zero width and height next to
    // `supportsExposureLock: true` — and both halves are believed downstream: the host derives a
    // field of view from `deriveFieldOfView(0, 0)` and the manager paces a burst expecting to pin
    // an exposure on a camera that is gone. Half an answer is worse than none, because none is a
    // state the core has a word for.
    // Through `fakeTrack`, which is the one model of an ended track in this file. It was written
    // inline here first, with `getSettings()` answering `{}` — and a reviewer pointed out that the
    // repo then carried two models of the same browser behaviour, disagreeing about the half this
    // test is named for: an ended track drops the *geometry* and keeps the mode strings, which is
    // what makes the answer a mixture rather than an absence. Two models is how one of them
    // quietly stops matching the browser.
    const track = fakeTrack({
      capabilities: { torch: true, exposureMode: ['continuous', 'manual'] },
      initial: { width: 1280, height: 720, frameRate: 30 },
    });
    const camera = createCameraAccess(mediaWith(track) as never);
    const opened = await camera.open({ preferRearCamera: true });
    expect(opened.ok && opened.value.supportsExposureLock).toBe(true);

    track.end();
    const now = camera.capabilities();
    expect(now.maxWidth, 'geometry').toBe(0);
    expect(now.maxBurstFps, 'rate').toBe(0);
    expect(now.supportsExposureLock,
      'a dead track was still promising an exposure lock').toBe(false);
    expect(now.supportsTorch, 'and a torch').toBe(false);
  });

  it('treats a rate that is not a measurement as no answer at all', async () => {
    // The guard is `typeof rate === 'number' && Number.isFinite(rate) && rate > 0`, and only the
    // ordinary case and the absent one were driven — so a reviewer deleted the whole condition and
    // all 43 camera tests stayed green.
    //
    // Each of these reaches the core as `maxBurstFps`, which floors a burst's interval and settle.
    // A NaN or a zero would become a period the manager has to decide what to do with; zero is the
    // contract's word for "the platform will not say", and that is the honest thing to send for a
    // track that answered with something that is not a rate.
    for (const frameRate of [Number.NaN, Number.POSITIVE_INFINITY, 0, -30, '30' as unknown]) {
      const camera = createCameraAccess(fakeMedia({
        stream: {
          getVideoTracks: () => [{
            getSettings: () => ({ width: 1280, height: 720, frameRate }),
            getCapabilities: () => ({}),
            stop: vi.fn(),
          }],
          getTracks: () => [{ stop: vi.fn() }],
        },
      }) as never);
      const result = await camera.open({ preferRearCamera: true });
      expect(result.ok).toBe(true);
      if (result.ok) {
        expect(result.value.maxBurstFps, `a frameRate of ${String(frameRate)} became a real rate`)
          .toBe(0);
      }
    }
  });

  it('says nothing about the rate rather than guessing when the track does not report one', async () => {
    // Zero is the contract's word for "the platform will not say", and the core reads it as "no
    // floor here". A default invented in the adapter would slow every burst on the browsers that
    // decline to answer, which the core's own test says is most of them.
    const camera = createCameraAccess(fakeMedia({}) as never);
    const result = await camera.open({ preferRearCamera: true });
    expect(result.ok).toBe(true);
    if (result.ok) expect(result.value.maxBurstFps).toBe(0);
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
  /**
   * A track with no `getCapabilities` at all. The method is optional and browsers differ; which
   * ones omit it is not something this repo has measured, and the code under test does not care —
   * what it must do is treat an absent method as "nothing reported" rather than as an error.
   */
  omitCapabilitiesApi?: boolean;
  /** A `getCapabilities` that throws instead of answering. */
  capabilitiesThrow?: string;
} = {}) {
  const applied: unknown[] = [];
  let settings: Record<string, unknown> = { width: 1920, height: 1080, ...options.initial };
  // Live until a test ends it. Modelled because an ended track is not a missing one and the two
  // answer very differently: `getSettings()` drops the geometry and keeps the mode strings, and
  // `applyConstraints` rejects.
  let readyState = 'live';
  let endOnWrite = false;
  const track = {
    applied,
    end() {
      readyState = 'ended';
      settings = Object.fromEntries(
        Object.entries(settings).filter(([key]) => !['width', 'height', 'frameRate'].includes(key)));
    },
    get readyState() { return readyState; },
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
    // Ends the moment the next constraint reaches it, which is how a track pulled away mid-write
    // behaves: the call in flight rejects and so does every one after it.
    endOnNextConstraint() { endOnWrite = true; },
    async applyConstraints(constraints: unknown) {
      applied.push(constraints);
      if (endOnWrite) this.end();
      if (readyState === 'ended') {
        // What Chromium does. Swallowed by `ask`, which is right — a camera that will not take a
        // constraint is a supported outcome — and is exactly why the read-back has to be of
        // something alive.
        const error = new Error('the track has ended');
        error.name = 'InvalidStateError';
        throw error;
      }
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
    // The camera works; the question about it is what cannot be put. Modelled rather than
    // attributed: `getCapabilities` is optional, and naming the browser that omits it would be
    // stating something this repo has not measured — the same guess this whole change removes.
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

  it('says nothing about the modes of a track that has ended', async () => {
    // ADR 0033: a refusal explained with the last camera's lists is worse than one explained with
    // nothing, and "the last camera" here is the same one — the page never calls `close()`, so a
    // track that simply ends leaves `offered` standing and the row goes on quoting a device that
    // is gone.
    const track = fakeTrack();
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });
    expect(camera.offeredModes().exposure).toEqual(['continuous', 'manual']);

    track.end();

    expect(camera.offeredModes().exposure, 'a dead track was still listing its modes').toBeNull();
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
  it('refuses a track that has ended rather than reading a lock off a corpse', async () => {
    // ADR 0022's rule is that the returned state is *observed* rather than acknowledged, and
    // observing a corpse is not observing. On an ended track `applyConstraints` rejects — which
    // `ask` swallows by design — while `getSettings()` keeps the last lock's mode strings. So the
    // read-back reports the locks the dead camera was holding, and a release that reached the
    // track nowhere comes back as `ok({exposure: true, …})`: success invented from rejections.
    //
    // The `!track` guard could not fire for this. `close()` is the only thing that clears the
    // adapter's stream and the page never calls it, so the dead track is still handed out.
    const track = fakeTrack();
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });
    const held = await camera.setLocks({ exposure: true, whiteBalance: true, focus: true });
    expect(held.ok && held.value.exposure, 'the arrangement never took a lock to lose').toBe(true);

    track.end();

    const released = await camera.setLocks({ exposure: false, whiteBalance: false, focus: false });
    expect(released.ok, 'a dead track answered a lock write').toBe(false);
    if (!released.ok) expect(released.status.code).toBe('CameraUnavailable');
  });

  it('refuses when the track ends while the locks are being written', async () => {
    // The entry guard above cannot see this and a reviewer proved it: the body awaits between
    // three and six `applyConstraints` calls — 120 ms each on the cameras this repo has measured,
    // and `writeLocks` allows a whole write three seconds — so a track that ends *inside* that
    // window walks straight past a check made before the first one.
    //
    // What comes out the other end is the invented success the entry guard was written to stop:
    // every rejection is swallowed by `ask` on purpose, and an ended track keeps the last lock's
    // mode strings, so the read-back reports three locks held by a camera that is gone. Which is
    // why the check that matters is on the read-back.
    const track = fakeTrack();
    const camera = createCameraAccess(mediaWith(track) as never);
    await camera.open({ preferRearCamera: true });
    const held = await camera.setLocks({ exposure: true, whiteBalance: true, focus: true });
    expect(held.ok && held.value.exposure, 'nothing was ever locked to lose').toBe(true);

    // Pulled away on the first constraint of the release, which is what a track being taken
    // mid-write looks like: this one rejects, and so does every one after it.
    track.endOnNextConstraint();

    const released = await camera.setLocks({ exposure: false, whiteBalance: false, focus: false });
    expect(released.ok, 'a track that died mid-write reported the locks it used to hold')
      .toBe(false);
    if (!released.ok) expect(released.status.detail).toContain('while its locks were being written');
  });

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
