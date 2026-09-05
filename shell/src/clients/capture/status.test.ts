// The capture client presents; it does not decide. These tests pin the one judgement it is
// allowed to make — turning a Result into words a person can act on — because "something went
// wrong" is the difference between a user fixing their permissions and closing the tab.
import { describe, expect, it } from 'vitest';

import { err, ok } from '../../access/result';
import {
  describeFailure, describeGuidanceFailure, describeLocks, formatCapabilities,
} from './status';

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
  // What `getCapabilities` said, which is a separate question from what the track then did.
  const listed = {
    exposure: ['continuous', 'manual'],
    whiteBalance: ['continuous', 'manual'],
    focus: ['continuous', 'manual'],
  };
  const unlisted = { exposure: null, whiteBalance: null, focus: null };

  it('names the locks that are actually holding', () => {
    expect(describeLocks(all, ok(all), listed)).toBe('exposure · white balance · focus');
  });

  it('says outright when the camera offers no manual modes at all', () => {
    // The reading this line exists for. With nothing locked, exposure and focus are the camera's
    // to move for the whole ~320 ms a burst spans — so a burst's frames can differ by more than
    // the scene does, and the sharpness spread across a cell is the camera hunting rather than
    // anything selection did. Without it that spread is unattributable.
    expect(describeLocks(none, ok(none), {
      exposure: ['continuous'], whiteBalance: ['continuous'], focus: ['continuous'],
    })).toMatch(/no manual modes/i);
  });

  it('separates a lock that was refused from one that was never asked for', () => {
    // Different faults. A refused lock is a camera that advertised a manual mode and then did not
    // take it, which is the case ADR 0022's read-back exists for; one never asked for is a camera
    // that said up front it could not. Reporting both as "no focus lock" would hide the first.
    const asked = { exposure: true, whiteBalance: false, focus: true };
    const got = { exposure: true, whiteBalance: false, focus: false };

    const line = describeLocks(asked, ok(got), listed);
    expect(line).toContain('exposure');
    expect(line).toMatch(/focus refused/i);
    expect(line).not.toMatch(/white balance refused/i);
  });

  it('names what the camera does offer when it refuses a lock', () => {
    // Two rounds of guessing at one Pixel got us `exposure refused` and no way to tell whether
    // the camera had a manual mode it would not take, or none to take. It listed one: this is
    // the camera contradicting itself, which is the case ADR 0022's read-back exists for.
    const line = describeLocks({ exposure: true, whiteBalance: false, focus: false },
      ok(none), { ...unlisted, exposure: ['continuous', 'manual'] });

    expect(line).toMatch(/exposure refused/i);
    expect(line).toContain('continuous, manual');
  });

  it('says a camera that lists only continuous has nothing to give', () => {
    // The other reading of the same refusal, and the one that ends the search: nothing was
    // withheld, there is no lock here to be had.
    const line = describeLocks({ exposure: true, whiteBalance: false, focus: false },
      ok(none), { ...unlisted, exposure: ['continuous'] });

    expect(line).toMatch(/exposure refused/i);
    expect(line).toContain('continuous');
    expect(line).not.toContain('manual');
  });

  it('does not pass off a browser that said nothing as a camera that offers nothing', () => {
    // Safari reports no enumerations at all, and Chromium omits these three keys. A row that
    // rendered that silence as "offers continuous" would be inventing the evidence.
    const line = describeLocks({ exposure: true, whiteBalance: false, focus: false },
      ok(none), unlisted);

    expect(line).toMatch(/exposure refused/i);
    expect(line).toMatch(/not reported/i);
    expect(line).not.toContain('continuous');
  });

  it('leaves a lock that is holding to speak for itself', () => {
    // The row is read at a glance between cells. What a held lock could have been is not a
    // question anybody has while it holds.
    const line = describeLocks(all, ok({ exposure: true, whiteBalance: true, focus: false }),
      listed);

    expect(line).toContain('exposure · white balance');
    expect(line).toMatch(/focus refused \(/);
    expect(line.match(/\(/g)).toHaveLength(1);
  });

  it('will not claim a camera has no manual modes when nothing was reported', () => {
    // The same collapse one line down. With every lock absent and no lists to read, "this camera
    // offers no manual modes" is a conclusion drawn from silence — and it is the sentence that
    // sends the next reader off to bracket for a camera nobody ever asked.
    const line = describeLocks(none, ok(none), unlisted);

    expect(line).not.toMatch(/offers no manual modes/i);
    expect(line).toMatch(/not report/i);
  });

  it('reports per control when the browser answered for some of them', () => {
    // A browser that fills in one key and not another. Neither blanket sentence is true, so the
    // row says which is which rather than picking whichever is closer.
    const line = describeLocks(none, ok(none),
      { exposure: ['continuous'], whiteBalance: null, focus: ['continuous'] });

    expect(line).toContain('exposure offers continuous');
    expect(line).toMatch(/white balance not reported/i);
  });

  it('says when the camera took a lock nothing asked it for', () => {
    // Not hypothetical: `advanced` constraints are best-effort, and a camera is free to settle on
    // manual for its own reasons. The burst is told what actually holds, so the line has to be
    // the same truth and not a copy of the request.
    expect(describeLocks(none, ok({ exposure: true, whiteBalance: false, focus: false }), listed))
      .toContain('exposure');
  });

  it('does not blame the locks when asking about them is what failed', () => {
    // `setLocks` fails outright when there is no camera to ask — a track pulled away mid-gesture,
    // a stream that ended. Rendering that as all-false makes this row say the camera has no
    // manual modes, which is the one sentence it exists to make trustworthy: it is the reading
    // that sends the next person after bracketing in Phase 2, for a camera that was never asked.
    const line = describeLocks(all, err('CameraUnavailable', 'CameraAccess', 'no camera open'),
      unlisted);

    expect(line).not.toMatch(/no manual modes/i);
    expect(line).not.toMatch(/refused/i);
    expect(line).toContain('no camera open');
  });

  it('falls back to the code when the failure carried no words', () => {
    // Every status has a code; detail is optional and empty for anything that failed without a
    // sentence to offer. An empty tail would read as the row having nothing to say.
    expect(describeLocks(all, err('CameraUnavailable', 'CameraAccess'), unlisted))
      .toContain('CameraUnavailable');
  });
});

describe('a tick that failed', () => {
  it('keeps the reason the component gave, not only the code', () => {
    // An iPhone reported `FrameStoreExhausted` and nothing else, and the store had said which of
    // two opposite problems it was: a sphere too large for the device, or a sink that refused a
    // write. One means capture less and the other means free some disk.
    expect(describeGuidanceFailure({
      code: 'FrameStoreExhausted',
      component: 'FrameStoreAccess',
      detail: 'allocation would exceed the heap ceiling, and the spill sink last refused a frame',
    })).toContain('spill sink last refused');
  });

  it('still names the code, which is the part that is the same everywhere', () => {
    expect(describeGuidanceFailure({
      code: 'FrameStoreExhausted', component: 'FrameStoreAccess', detail: 'no room',
    })).toContain('FrameStoreExhausted');
  });

  it('says just the code when there was nothing else to say', () => {
    const line = describeGuidanceFailure({ code: 'Internal', component: 'X', detail: '' });
    expect(line).toBe('Internal');
  });
});
