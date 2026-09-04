// The capture client presents; it does not decide. These tests pin the one judgement it is
// allowed to make — turning a Result into words a person can act on — because "something went
// wrong" is the difference between a user fixing their permissions and closing the tab.
import { describe, expect, it } from 'vitest';

import { err, ok } from '../../access/result';
import { describeFailure, describeLocks, formatCapabilities } from './status';

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
    }, true);
    expect(text).toMatch(/single-threaded/i);
  });

  it('reports the thread count when there is one', () => {
    const text = formatCapabilities({
      simd: true, threads: true, sharedMemory: true, hardwareConcurrency: 8,
    }, true);
    expect(text).toMatch(/8/);
  });

  it('calls out missing SIMD, which is a build mistake rather than a device limit', () => {
    const text = formatCapabilities({
      simd: false, threads: false, sharedMemory: false, hardwareConcurrency: 0,
    }, true);
    expect(text).toMatch(/no simd/i);
  });

  it('says when there is nowhere to spill, and stays quiet when there is', () => {
    // A capture with no spill tier is capped at what fits in RAM (ADR 0020). It will look like an
    // arbitrary refusal partway through a sphere unless the reason is on screen before it
    // happens — and it is a fact about the browser, so a bug report can carry it.
    const capabilities = {
      simd: true, threads: false, sharedMemory: false, hardwareConcurrency: 0,
    };
    expect(formatCapabilities(capabilities, false)).toMatch(/no spill/i);
    expect(formatCapabilities(capabilities, true)).not.toMatch(/spill/i);
  });
});

describe('Unsupported is not always about the camera', () => {
  it('explains the secure-origin case when the camera adapter raises it', () => {
    expect(describeFailure({
      code: 'Unsupported', component: 'CameraAccess', detail: 'no media devices',
    })).toMatch(/https/);
  });

  it('keeps the reason any other component gave', () => {
    // The burst path, the build pipeline and resume all report Unsupported with a reason worth
    // reading. Mapping the code alone replaced every one of them with an https message about a
    // camera that was not the problem.
    const detail = 'a burst takes time and cannot be made resident in advance; see ADR 0014';
    expect(describeFailure({
      code: 'Unsupported', component: 'BrowserCameraAccess', detail,
    })).toBe(detail);
    expect(describeFailure({
      code: 'Unsupported', component: 'PanoramaBuildManager',
      detail: 'nothing to build until Phase 2',
    })).toBe('nothing to build until Phase 2');
  });
});

describe('what the camera let the burst lock', () => {
  const none = { exposure: false, whiteBalance: false, focus: false };
  const all = { exposure: true, whiteBalance: true, focus: true };

  it('names the locks that are actually holding', () => {
    expect(describeLocks(all, ok(all))).toBe('exposure · white balance · focus');
  });

  it('says outright when the camera offers no manual modes at all', () => {
    // The reading this line exists for. With nothing locked, exposure and focus are the camera's
    // to move for the whole ~320 ms a burst spans — so a burst's frames can differ by more than
    // the scene does, and the sharpness spread across a cell is the camera hunting rather than
    // anything selection did. Without it that spread is unattributable.
    expect(describeLocks(none, ok(none))).toMatch(/no manual modes/i);
  });

  it('separates a lock that was refused from one that was never asked for', () => {
    // Different faults. A refused lock is a camera that advertised a manual mode and then did not
    // take it, which is the case ADR 0022's read-back exists for; one never asked for is a camera
    // that said up front it could not. Reporting both as "no focus lock" would hide the first.
    const asked = { exposure: true, whiteBalance: false, focus: true };
    const got = { exposure: true, whiteBalance: false, focus: false };

    const line = describeLocks(asked, ok(got));
    expect(line).toContain('exposure');
    expect(line).toMatch(/focus refused/i);
    expect(line).not.toMatch(/white balance refused/i);
  });

  it('says when the camera took a lock nothing asked it for', () => {
    // Not hypothetical: `advanced` constraints are best-effort, and a camera is free to settle on
    // manual for its own reasons. The burst is told what actually holds, so the line has to be
    // the same truth and not a copy of the request.
    expect(describeLocks(none, ok({ exposure: true, whiteBalance: false, focus: false })))
      .toContain('exposure');
  });

  it('does not blame the locks when asking about them is what failed', () => {
    // `setLocks` fails outright when there is no camera to ask — a track pulled away mid-gesture,
    // a stream that ended. Rendering that as all-false makes this row say the camera has no
    // manual modes, which is the one sentence it exists to make trustworthy: it is the reading
    // that sends the next person after bracketing in Phase 2, for a camera that was never asked.
    const line = describeLocks(all, err('CameraUnavailable', 'CameraAccess', 'no camera open'));

    expect(line).not.toMatch(/no manual modes/i);
    expect(line).not.toMatch(/refused/i);
    expect(line).toContain('no camera open');
  });

  it('falls back to the code when the failure carried no words', () => {
    // Every status has a code; detail is optional and empty for anything that failed without a
    // sentence to offer. An empty tail would read as the row having nothing to say.
    expect(describeLocks(all, err('CameraUnavailable', 'CameraAccess')))
      .toContain('CameraUnavailable');
  });
});
