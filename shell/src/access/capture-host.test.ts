// The page half of the camera and motion ports.
//
// The core reads these synchronously, so everything here is state the page has already
// established (ADR 0014). The one piece of real arithmetic is the field of view, which the
// browser does not report and the coverage plan cannot be built without.
import { describe, expect, it } from 'vitest';

import { createCaptureHost, deriveFieldOfView } from './capture-host';

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
    host.setCamera({ maxWidth: 1920, maxHeight: 1080, supportsTorch: true });
    expect(host.cameraOpen()).toBe(true);
    const caps = host.cameraCapabilities();
    expect(caps.maxWidth).toBe(1920);
    expect(caps.horizontalFovDeg).toBeGreaterThan(0);
    expect(caps.verticalFovDeg).toBeGreaterThan(0);
  });

  it('forgets the camera when the page closes it', () => {
    // A stale capability set would let the core plan a capture against a camera that is gone.
    const host = createCaptureHost();
    host.setCamera({ maxWidth: 1920, maxHeight: 1080, supportsTorch: false });
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
