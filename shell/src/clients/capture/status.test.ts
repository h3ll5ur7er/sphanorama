// The capture client presents; it does not decide. These tests pin the one judgement it is
// allowed to make — turning a Result into words a person can act on — because "something went
// wrong" is the difference between a user fixing their permissions and closing the tab.
import { describe, expect, it } from 'vitest';

import { describeFailure, formatCapabilities } from './status';

describe('describeFailure', () => {
  it('tells a user who declined the camera what to do about it', () => {
    const message = describeFailure({ code: 'SensorPermissionDenied', component: 'CameraAccess', detail: '' });
    expect(message).toMatch(/permission/i);
    expect(message).not.toMatch(/SensorPermissionDenied/);
  });

  it('distinguishes a missing camera from a declined one', () => {
    const declined = describeFailure({ code: 'SensorPermissionDenied', component: 'CameraAccess', detail: '' });
    const missing = describeFailure({ code: 'CameraUnavailable', component: 'CameraAccess', detail: '' });
    expect(declined).not.toEqual(missing);
  });

  it('names the https requirement for an insecure origin, which is ours to fix not the user\'s', () => {
    const message = describeFailure({ code: 'Unsupported', component: 'CameraAccess', detail: 'needs https' });
    expect(message).toMatch(/https/i);
  });

  it('falls back to something honest for a code it has no words for', () => {
    const message = describeFailure({ code: 'Internal', component: 'X', detail: 'boom' });
    expect(message).toContain('boom');
  });
});

describe('formatCapabilities', () => {
  it('says plainly when the core is running single-threaded', () => {
    // The default deployment has no threads (ADR 0011). Showing that is how a slow build gets
    // diagnosed from a screenshot instead of a debugging session.
    const text = formatCapabilities({
      simd: true, threads: false, sharedMemory: false, hardwareConcurrency: 0,
    });
    expect(text).toMatch(/single-threaded/i);
  });

  it('reports the thread count when there is one', () => {
    const text = formatCapabilities({
      simd: true, threads: true, sharedMemory: true, hardwareConcurrency: 8,
    });
    expect(text).toMatch(/8/);
  });

  it('calls out missing SIMD, which is a build mistake rather than a device limit', () => {
    const text = formatCapabilities({
      simd: false, threads: false, sharedMemory: false, hardwareConcurrency: 0,
    });
    expect(text).toMatch(/no simd/i);
  });
});
