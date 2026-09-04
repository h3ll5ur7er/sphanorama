// The resident half of the camera and motion ports, in the context the core runs in.
//
// The core reads these synchronously, so everything here is state the page has already
// established (ADR 0014) and pushed across (ADR 0019). The one piece of real arithmetic is the
// field of view, which the browser does not report and the coverage plan cannot be built without.
import { describe, expect, it } from 'vitest';

import {
  MOTION_BUFFER_LIMIT, MOTION_SAMPLE_DOUBLES, createCaptureHost, deriveFieldOfView,
  flattenImuSamples, stopCameraStream,
} from './capture-host';

describe('deriveFieldOfView', () => {
  it('keeps the horizontal estimate it was given', () => {
    expect(deriveFieldOfView(1920, 1080, 66).horizontalFovDeg).toBeCloseTo(66, 6);
  });

  it('derives the vertical angle from the real aspect ratio', () => {
    // The browser reports the resolution it settled on, so the aspect is genuine even though the
    // horizontal angle is an assumption. Scaling one by the other beats assuming both.
    const wide = deriveFieldOfView(1920, 1080, 66);
    const tall = deriveFieldOfView(1080, 1920, 66);
    expect(wide.verticalFovDeg).toBeLessThan(wide.horizontalFovDeg);
    expect(tall.verticalFovDeg).toBeGreaterThan(wide.verticalFovDeg);
  });

  it('is consistent with the tangent relationship rather than a linear scale', () => {
    // Angles do not scale linearly with sensor dimensions; treating them as if they did would
    // overstate the vertical angle and leave gaps between rings.
    const { horizontalFovDeg, verticalFovDeg } = deriveFieldOfView(1920, 1080, 66);
    const toRad = Math.PI / 180;
    const expected = 2 * Math.atan(Math.tan((horizontalFovDeg * toRad) / 2) * (1080 / 1920));
    expect(verticalFovDeg).toBeCloseTo(expected / toRad, 6);
  });

  it('falls back to a plausible lens when the browser reports no resolution', () => {
    // Some devices report zeroes until the first frame arrives; a zero field of view would make
    // the planner refuse to plan at all.
    const fov = deriveFieldOfView(0, 0, 66);
    expect(fov.horizontalFovDeg).toBeGreaterThan(0);
    expect(fov.verticalFovDeg).toBeGreaterThan(0);
  });

  it('never reports an angle at or past a half turn', () => {
    for (const [w, h] of [[1, 10000], [10000, 1], [640, 480]]) {
      const fov = deriveFieldOfView(w, h, 66);
      expect(fov.verticalFovDeg).toBeGreaterThan(0);
      expect(fov.verticalFovDeg).toBeLessThan(180);
    }
  });
});

describe('capture host', () => {
  it('reports no camera until the page opens one', () => {
    const host = createCaptureHost();
    expect(host.cameraOpen()).toBe(false);
    expect(host.cameraCapabilities().maxWidth).toBe(0);
  });

  it('reports what the page opened', () => {
    const host = createCaptureHost();
    host.setCamera({
      maxWidth: 1920, maxHeight: 1080, supportsTorch: true,
      supportsExposureLock: false, supportsWhiteBalanceLock: false,
      supportsFocusLock: false,
    });
    expect(host.cameraOpen()).toBe(true);
    const caps = host.cameraCapabilities();
    expect(caps.maxWidth).toBe(1920);
    expect(caps.horizontalFovDeg).toBeGreaterThan(0);
    expect(caps.verticalFovDeg).toBeGreaterThan(0);
  });

  it('forgets the camera when the page closes it', () => {
    // A stale capability set would let the core plan a capture against a camera that is gone.
    const host = createCaptureHost();
    host.setCamera({
      maxWidth: 1920, maxHeight: 1080, supportsTorch: false,
      supportsExposureLock: false, supportsWhiteBalanceLock: false,
      supportsFocusLock: false,
    });
    host.clearCamera();
    expect(host.cameraOpen()).toBe(false);
  });

  it('reports motion as absent until the page starts it', () => {
    const host = createCaptureHost();
    expect(host.motionCapability()).toBe('None');
  });

  it('reports the motion capability the page negotiated', () => {
    // Absence is a supported configuration, so this has to distinguish "not started" from
    // "started and there is nothing there".
    const host = createCaptureHost();
    host.setMotion('OrientationOnly');
    expect(host.motionCapability()).toBe('OrientationOnly');
    host.setMotion('None');
    expect(host.motionCapability()).toBe('None');
  });
});

describe('the motion buffer the core drains', () => {
  const sample = (timestampNs: number) => ({
    timestampNs,
    hasAngularVelocity: true,
    angularVelocity: { x: 1, y: 2, z: 3 },
    acceleration: { x: 4, y: 5, z: 6 },
    hasMagnetometer: false,
    magneticField: { x: 0, y: 0, z: 0 },
    hasOrientation: true,
    orientation: { w: 1, x: 0, y: 0, z: 0 },
  });

  it('hands back every field in the order the core reads them', () => {
    // The order is the wire format for this port. It is flat doubles rather than the generated
    // codec because the codec exists for the facade, and a second marshalling path inside a
    // resource access is exactly what the port pattern is meant to avoid.
    const host = createCaptureHost();
    host.pushMotion(flattenImuSamples([sample(7)]));
    expect(host.motionDrain(4)).toEqual([
      7, 1, 1, 2, 3, 4, 5, 6, 0, 0, 0, 0, 1, 1, 0, 0, 0,
    ]);
  });

  it('drains what it hands over, so the core never integrates a sample twice', () => {
    const host = createCaptureHost();
    host.pushMotion(flattenImuSamples([sample(1), sample(2)]));
    expect(host.motionDrain(8)).toHaveLength(2 * MOTION_SAMPLE_DOUBLES);
    expect(host.motionDrain(8)).toEqual([]);
  });

  it('respects the batch the core asked for and keeps the rest', () => {
    const host = createCaptureHost();
    host.pushMotion(flattenImuSamples([sample(1), sample(2), sample(3)]));
    expect(host.motionDrain(2)).toHaveLength(2 * MOTION_SAMPLE_DOUBLES);
    expect(host.motionDrain(8)).toHaveLength(1 * MOTION_SAMPLE_DOUBLES);
  });

  it('drops the oldest rather than growing without bound', () => {
    // A capture loop that stalls must not be able to kill the tab, and a stale orientation is
    // worthless anyway — the newest samples are the ones worth keeping.
    const host = createCaptureHost();
    host.pushMotion(flattenImuSamples(
      Array.from({ length: MOTION_BUFFER_LIMIT + 10 }, (_u, i) => sample(i))));
    const drained = host.motionDrain(MOTION_BUFFER_LIMIT + 10);
    expect(drained).toHaveLength(MOTION_BUFFER_LIMIT * MOTION_SAMPLE_DOUBLES);
    expect(drained[0]).toBe(10);   // the first ten were dropped, not the last ten
  });
});

describe('closing and resetting', () => {
  it('stops every track when the core closes the camera', () => {
    // Video alone is not enough: any live track keeps the indicator lit, and a user who ended a
    // capture reasonably reads that as the app still watching them.
    const stopped: string[] = [];
    const track = (kind: string) => ({ kind, stop: () => stopped.push(kind) });
    const stream = { getTracks: () => [track('video'), track('audio')] } as unknown as MediaStream;

    stopCameraStream(stream);
    expect(stopped).toEqual(['video', 'audio']);
  });

  it('asks whoever holds the stream to stop it, and forgets the camera either way', () => {
    // The host cannot stop a MediaStream from the worker it runs in, so Close is a request. The
    // capabilities go regardless, or the core could plan a capture against a camera that is gone.
    let asked = 0;
    const host = createCaptureHost({ onCloseCamera: () => { asked += 1; } });
    host.setCamera({
      maxWidth: 640, maxHeight: 480, supportsTorch: false,
      supportsExposureLock: false, supportsWhiteBalanceLock: false,
      supportsFocusLock: false,
    });
    expect(host.cameraOpen()).toBe(true);

    host.closeCamera();
    expect(asked).toBe(1);
    expect(host.cameraOpen()).toBe(false);
  });

  it('survives a close with nobody holding a stream', () => {
    const host = createCaptureHost();
    expect(() => host.closeCamera()).not.toThrow();
  });

  it('drops buffered samples when the sensor stops', () => {
    // Samples from a finished session describe a pose nobody asked for; handing them to the next
    // one would start it looking the wrong way.
    const host = createCaptureHost();
    host.pushMotion(flattenImuSamples([{
      timestampNs: 1, hasAngularVelocity: false,
      angularVelocity: { x: 0, y: 0, z: 0 }, acceleration: { x: 0, y: 0, z: 0 },
      hasMagnetometer: false, magneticField: { x: 0, y: 0, z: 0 },
      hasOrientation: true, orientation: { w: 1, x: 0, y: 0, z: 0 },
    }]));
    host.resetMotion();
    expect(host.motionDrain(8)).toEqual([]);
  });
});

describe('the resident preview frame', () => {
  const frame = (width: number, height: number, fill = 1) => ({
    width, height, bytes: new Uint8Array(width * height * 4).fill(fill),
  });

  it('reports no frame until one has been pushed', () => {
    // The core asks through a synchronous port, so "not yet" has to be an answer rather than a
    // wait. A burst that peeks before the first frame arrives is told, and abandons.
    const host = createCaptureHost();
    expect(host.previewFrame()).toBeNull();
  });

  it('holds the latest frame for the core to read', () => {
    const host = createCaptureHost();
    host.setPreviewFrame(frame(4, 3, 0xAB));

    const held = host.previewFrame();
    expect(held).not.toBeNull();
    expect(held!.width).toBe(4);
    expect(held!.height).toBe(3);
    expect(held!.bytes[0]).toBe(0xAB);
  });

  it('replaces the previous frame rather than queueing', () => {
    // Latest, not next: a queue would hand a burst frames from before the phone was aimed, and
    // the manager paces itself on the clock rather than on how many arrived.
    const host = createCaptureHost();
    host.setPreviewFrame(frame(4, 3, 1));
    host.setPreviewFrame(frame(8, 6, 2));

    expect(host.previewFrame()!.width).toBe(8);
    expect(host.previewFrame()!.bytes[0]).toBe(2);
  });

  it('forgets the frame when the camera closes', () => {
    // A frame from a camera that is gone would let a burst armed afterwards capture the last
    // thing the old stream saw, which is a picture of somewhere the phone no longer points.
    const host = createCaptureHost();
    host.setPreviewFrame(frame(4, 3));
    host.closeCamera();

    expect(host.previewFrame()).toBeNull();
  });

  it('forgets the frame when the camera is cleared', () => {
    const host = createCaptureHost();
    host.setPreviewFrame(frame(4, 3));
    host.clearCamera();

    expect(host.previewFrame()).toBeNull();
  });

  it('refuses a frame whose bytes do not match its dimensions', () => {
    // The length is what the core sizes its copy from. A short buffer would have it read past
    // the end of the transferred memory, which is the one failure here that is not a bad picture.
    const host = createCaptureHost();
    host.setPreviewFrame({ width: 4, height: 3, bytes: new Uint8Array(4 * 3 * 4 - 1) });

    expect(host.previewFrame()).toBeNull();
  });
});

describe('which locks the camera is holding', () => {
  it('reports nothing locked before anything has been applied', () => {
    const host = createCaptureHost();
    expect(host.cameraLocks()).toEqual({ exposure: false, whiteBalance: false, focus: false });
  });

  it('reports what the page confirmed, not what it asked for', () => {
    // The distinction this whole path exists for: applyConstraints resolving is not the mode
    // changing, so what crosses is the state read back off the track (ADR 0022).
    const host = createCaptureHost();
    host.setCameraLocks({ exposure: true, whiteBalance: false, focus: true });

    expect(host.cameraLocks()).toEqual({ exposure: true, whiteBalance: false, focus: true });
  });

  it('forgets the locks when the camera closes', () => {
    // A lock belongs to a track. Reporting one held after the stream is gone would let the next
    // session arm a burst believing an exposure was fixed by a camera that no longer exists.
    const host = createCaptureHost();
    host.setCameraLocks({ exposure: true, whiteBalance: true, focus: true });
    host.closeCamera();

    expect(host.cameraLocks().exposure).toBe(false);
  });

  it('forgets the locks when the camera is cleared', () => {
    const host = createCaptureHost();
    host.setCameraLocks({ exposure: true, whiteBalance: true, focus: true });
    host.clearCamera();

    expect(host.cameraLocks().focus).toBe(false);
  });

  it('asks the page to release the locks, since only it holds the track', () => {
    // The mirror of closeCamera: the core can ask, and the page is the only side that can act.
    // Posted rather than awaited, because the burst that held the lock is already over.
    const released: number[] = [];
    const host = createCaptureHost({ onReleaseCameraLocks: () => released.push(1) });
    host.setCameraLocks({ exposure: true, whiteBalance: true, focus: true });

    host.releaseCameraLocks();

    expect(released).toHaveLength(1);
    // Cleared here too, not only when the page gets round to it: the core asked, and a port that
    // kept reporting the lock held would let the next burst arm on a stale confirmation.
    expect(host.cameraLocks().exposure).toBe(false);
  });

  it('carries the lock capabilities the page reported', () => {
    const host = createCaptureHost();
    host.setCamera({
      maxWidth: 1920, maxHeight: 1080, supportsTorch: false,
      supportsExposureLock: true, supportsWhiteBalanceLock: false, supportsFocusLock: true,
    });

    const capabilities = host.cameraCapabilities();
    expect(capabilities.supportsExposureLock).toBe(true);
    expect(capabilities.supportsFocusLock).toBe(true);
  });

  it('claims no lock support for a camera it does not have', () => {
    const host = createCaptureHost();
    expect(host.cameraCapabilities().supportsExposureLock).toBe(false);
  });
});
