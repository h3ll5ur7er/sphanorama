// End-to-end against the built bundle in dist/, not the dev server: what ships is what is tested.
//
// Chromium's fake media device stands in for a camera, so the enable path runs for real rather
// than being mocked out at exactly the point where it usually breaks.
import { test, expect } from '@playwright/test';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';

import { startServer } from '../../tools/static_server.mjs';
import { GRAB_MAX_EDGE } from '../src/access/preview-frame.ts';
import { PREVIEW_MAX_EDGE } from '../src/clients/review/panel.ts';
import { quaternionFromDeviceOrientation } from '../src/access/orientation.ts';

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const dist = resolve(repoRoot, 'dist');

test.skip(!existsSync(dist), 'run: npm run build');

// The same base the bundle was built with, so the suite exercises the mount the deploy will
// actually use: Pages serves a project site from /<repo>/, and a bundle built for that prefix
//404s every asset when served at /. Verifying a deploy against the wrong mount verifies nothing.
const basePath = process.env.SPHANORAMA_BASE ?? '/';

async function serve() {
  return startServer({ roots: [dist], basePath });
}

/**
 * Waits for the viewfinder to be delivering frames, which is not the same as being able to aim.
 *
 * Guidance says `HoldStill` once the camera is inside a cell's cone — a fact about where the
 * camera is pointing, not about whether it is producing frames. The grabber refuses a video with no data or no dimensions and the loop only grabs at all
 * once a burst is armed, so a burst armed before the first frame arrives spends its whole settle
 * (ADR 0032) waiting for one that is not there and then fails with `CameraUnavailable`. On a loaded
 * runner that window is wide enough to fail a test, which is how this was found: one job, one
 * assertion, "the page has not grabbed a frame yet".
 *
 * The same condition the grabber itself checks (`preview-frame.ts`'s `createFrameGrabber`, both
 * guards), so this waits for the thing that actually has to be true rather than for a length of
 * time.
 *
 * It is usually satisfied the moment it is asked: this runner has the video at `readyState 4` and
 * its full size by the time `#stage` says `capturing`, so the wait returns in single-digit
 * milliseconds and no assertion here depends on it. That is what it is for. `srcObject` is
 * assigned several worker round trips before the stage text changes, so the ordering is not
 * guaranteed by anything, and the failure it prevents — `CameraUnavailable`, the page has not
 * grabbed a frame yet — is one CI has actually produced on a branch that touched none of this.
 */
/**
 * Every candidate the session holds, across every cell.
 *
 * Summed rather than short-circuited on the first non-empty cell: "did this capture bank anything
 * at all" is the question both callers are asking, and a loop that returns the first cell's count
 * answers a narrower one.
 */
async function countCandidates(page) {
  return page.evaluate(async () => {
    const plan = await window.sphanoramaCore.captureSession.getPlan();
    let total = 0;
    for (const node of plan.value.nodes) {
      const got = await window.sphanoramaCore.captureSession.candidates(node.id);
      if (got.ok) total += got.value.length;
    }
    return total;
  });
}

const RAD_TO_DEG = 180 / Math.PI;

/**
 * The device-orientation triple that would make the viewfinder look along `target`.
 *
 * The inverse of the adapter's own conversion, and it exists so a test can point the phone at a
 * cell the plan actually has instead of guessing angles until one sticks. `toCoreFrame` composes
 * `EARTH_TO_CORE ⊗ device ⊗ screen`; `screen.orientation.angle` is 0 on this runner, so the
 * device attitude is `EARTH_TO_CORE⁻¹ ⊗ target` and what is left is reading intrinsic Z-X'-Y''
 * angles back out of it — the same decomposition `DeviceOrientationEvent` is defined by.
 *
 * The caller checks the result against the real conversion rather than trusting this, which is
 * what keeps a wrong sign here from becoming a fifteen-second timeout somewhere else.
 */
function deviceOrientationLookingAt(target) {
  // EARTH_TO_CORE⁻¹ ⊗ target. Written out because importing a quaternion library to conjugate one
  // constant is more machinery than the constant.
  const h = Math.SQRT1_2;
  const q = {
    w: h * (target.w - target.x),
    x: h * (target.w + target.x),
    y: h * (target.y - target.z),
    z: h * (target.y + target.z),
  };

  // The rotation matrix entries the decomposition needs, from R = Rz(alpha) Rx(beta) Ry(gamma).
  const m01 = 2 * (q.x * q.y - q.w * q.z);
  const m11 = 1 - 2 * (q.x * q.x + q.z * q.z);
  const m20 = 2 * (q.x * q.z - q.w * q.y);
  const m21 = 2 * (q.y * q.z + q.w * q.x);
  const m22 = 1 - 2 * (q.x * q.x + q.y * q.y);

  const beta = Math.asin(Math.max(-1, Math.min(1, m21)));

  // Gimbal lock, and on this plan it is not an edge case — it is the horizon.
  //
  // At `beta = ±90°` the Z and Y rotations act about the same axis, so only their sum (or
  // difference) is determined and `atan2(-m01, m11)` reads `atan2(0, 0)`: the azimuth is thrown
  // away and every cell on the ring maps to the same triple. `Math.asin`'s clamp does not help —
  // it guards against drifting *past* 1, not against being at it.
  //
  // Elevation zero is exactly `beta = 90°` in this frame, so the whole horizon ring is degenerate.
  // A reviewer measured it: azimuth 45° round-trips to |dot| 0.9239, 90° to 0.7071, 180° to
  // 0.0000, and cells 13-19 of the shipped plan fail the check below. It had not bitten only
  // because `aimAtACell` walks the plan in order and the rings engine emits a pole first.
  //
  // The convention out is the usual one: fold the free rotation into `alpha` and take `gamma` as
  // zero. Roll about the view axis is what `gamma` contributes here, and the tests that aim do
  // not care how the phone is rolled — the ones that do care dispatch their own triples.
  if (m21 > 1 - 1e-7) {
    // One sign, not two. `m21` works out to the cosine of the cell's elevation, and a plan's
    // elevations live in [-90°, 90°], so it is never negative — a `beta = -90°` branch was
    // written here and could not be reached by any plan, which a reviewer showed by deleting it
    // with every browser test still green. A branch no input reaches is not defence, it is a
    // second thing to keep true.
    const m00 = 1 - 2 * (q.y * q.y + q.z * q.z);
    const m02 = 2 * (q.x * q.z + q.w * q.y);
    return { alpha: Math.atan2(m02, m00) * RAD_TO_DEG, beta: beta * RAD_TO_DEG, gamma: 0 };
  }

  return {
    alpha: Math.atan2(-m01, m11) * RAD_TO_DEG,
    beta: beta * RAD_TO_DEG,
    gamma: Math.atan2(-m20, m22) * RAD_TO_DEG,
  };
}

/**
 * Points the phone at a cell that still needs shooting, and waits for the core to agree.
 *
 * This replaces waiting for `#capture` to become enabled, which is what most of this suite used
 * as its "the app is ready to capture" signal until ADR 0044 took the button away. That signal
 * had always been a slightly dishonest one: it went enabled because this runner reports *no*
 * orientation until a test dispatches one, so every assertion under it ran the sensorless path —
 * the one path the app no longer has.
 *
 * One event, not a stream, and that is deliberate: an attitude anchors the pose and guidance
 * starts naming the cell the camera is inside, while the dwell stays at zero because it only
 * advances on ticks a sample arrived on (ADR 0043). So a test that wants to arm a burst itself
 * still can, and only a test that keeps dispatching gets one fired for it.
 *
 * The cell is read from the plan the core made and the candidates it holds, so calling this again
 * after a burst aims at a different cell rather than back at the one just filled.
 */
async function aimAtACell(page) {
  // The listener has to be installed before an event can reach it, and it is installed several
  // worker round trips after `#stage` says the session started. Without this wait the dispatch
  // below lands in an empty room and the assertion that follows waits out its whole timeout.
  await expect(page.locator('#motion-state')).toContainText('DeviceOrientation', {
    timeout: 15000,
  });

  const target = await page.evaluate(async () => {
    const plan = await window.sphanoramaCore.captureSession.getPlan();
    if (!plan.ok) return null;
    for (const node of plan.value.nodes) {
      const got = await window.sphanoramaCore.captureSession.candidates(node.id);
      if (got.ok && got.value.length === 0) {
        return { id: node.id, orientation: node.targetOrientation };
      }
    }
    return null;
  });
  expect(target, 'the plan has no cell left to aim at').not.toBeNull();

  const { alpha, beta, gamma } = deviceOrientationLookingAt(target.orientation);
  // Checked against the adapter's own conversion, not assumed. A sign error in the inverse would
  // otherwise show up as whichever assertion came next timing out, fifteen seconds later and in
  // a test about something else entirely.
  const round = quaternionFromDeviceOrientation(alpha, beta, gamma, 0);
  const dot = round.w * target.orientation.w + round.x * target.orientation.x
    + round.y * target.orientation.y + round.z * target.orientation.z;
  expect(Math.abs(dot), `aimed at ${JSON.stringify(target.orientation)}`).toBeGreaterThan(0.9999);

  await page.evaluate(({ alpha: a, beta: b, gamma: g }) => {
    window.dispatchEvent(new DeviceOrientationEvent('deviceorientation',
      { alpha: a, beta: b, gamma: g }));
  }, { alpha, beta, gamma });
  await expect(page.locator('#guidance')).toContainText(/hold still/, { timeout: 15000 });
  // The angles come back with the cell so a caller can keep dispatching the same attitude — which
  // is what maturing a dwell takes, since it only advances on ticks a sample arrived on.
  return { id: target.id, alpha, beta, gamma };
}

async function viewfinderIsLive(page) {
  await page.waitForFunction(() => {
    const video = document.querySelector('video');
    return video !== null && video.readyState >= 2
      && video.videoWidth > 0 && video.videoHeight > 0;
  }, null, { timeout: 15000 });
}

test('loads the core and reports its capabilities', async ({ page }) => {
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    // The default deployment has no threads (ADR 0011). Showing it makes a slow build
    // diagnosable from a screenshot.
    await expect(page.locator('#core-caps')).toContainText('single-threaded');
    await expect(page.locator('#core-caps')).toContainText('SIMD');
    // The spill tier is announced only when it is *missing* (ADR 0020), so its absence from this
    // line is the assertion: it says the worker opened an OPFS sync access handle for real. That
    // is the platform risk ADR 0019 took — the handle is worker-only, and the whole reason the
    // core moved off the main thread — so a browser that stopped supporting it must fail here
    // rather than at the moment a capture runs out of memory.
    await expect(page.locator('#core-caps')).not.toContainText('no spill tier');
  } finally {
    await server.close();
  }
});

test('a second tab open on the app gets a spill tier of its own', async ({ page, context }) => {
  // Reported from a phone: the app said "no spill tier" until every other tab running it was
  // closed. A sync access handle is exclusive, and both tabs were asking for the same fixed
  // filename — so the second one captured a sphere capped at RAM and said nothing about why.
  // Two pages in one context share the origin's private file system, which is the whole test.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await expect(page.locator('#core-caps')).not.toContainText('no spill tier');

    const second = await context.newPage();
    await second.goto(server.appUrl);
    await expect(second.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await expect(second.locator('#core-caps')).not.toContainText('no spill tier');
    // And the first one is untouched: the newcomer's sweep of abandoned files must not have
    // taken the file the tab beside it is still spilling to.
    await expect(page.locator('#core-caps')).not.toContainText('no spill tier');
  } finally {
    await server.close();
  }
});

test('the page says which build it is', async ({ page }) => {
  // A screenshot from a phone is the only evidence some of this project has — the OPFS spill
  // tier, the lens the camera chose, the cell count — and every one of those readings is worth
  // nothing if nobody can tell which commit produced it. That is not hypothetical: a device
  // report and a fix for it crossed in flight once already, and the only way to tell was to ask.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    // A short hash, optionally marked dirty — never the placeholder, which would be a build that
    // cannot say where it came from.
    await expect(page.locator('#build')).toHaveText(/^[0-9a-f]{7,40}(-dirty)?$/);
  } finally {
    await server.close();
  }
});

test('the candidate you picked is still readable', async ({ page }) => {
  // A rule that painted the chosen row's background and its text the same colour rendered it as
  // a solid black bar — the one row a user had just chosen was the one row they could not read.
  // Unit tests cannot see it: it needs a browser to resolve the cascade, and `currentColor`
  // resolving against the element's *own* colour is exactly the kind of thing that only shows up
  // once something computes it.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });

    const painted = await page.evaluate(() => {
      const button = document.createElement('button');
      button.type = 'button';
      button.setAttribute('aria-pressed', 'true');
      button.textContent = 'a candidate';
      document.querySelector('#strip').append(button);
      const style = getComputedStyle(button);
      const seen = { background: style.backgroundColor, ink: style.color };
      button.remove();
      return seen;
    });
    expect(painted.background).not.toBe(painted.ink);
  } finally {
    await server.close();
  }
});

test('enabling starts the camera and the viewfinder gets a stream', async ({ page }) => {
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();

    await expect(page.locator('#camera-state')).not.toHaveText('—', { timeout: 15000 });
    const hasStream = await page.evaluate(
      () => document.querySelector('video').srcObject !== null);
    expect(hasStream).toBe(true);
  } finally {
    await server.close();
  }
});

test('the camera is opened at the resolution the frames are stored at', async ({ page }) => {
  // Left unasked, getUserMedia hands back the browser's own default rather than the camera's
  // best: 640x480 in Chromium, against a grabber that keeps 1280 on the long edge. So the cap
  // that exists to bound memory was bounding nothing, and every frame the core scored was a
  // quarter of the pixels it had budgeted for. What the camera settled on is on screen, because
  // the coverage plan is sized from it.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#camera-state')).not.toHaveText('—', { timeout: 15000 });

    const shown = await page.locator('#camera-state').textContent();
    const [width, height] = (shown ?? '').split('\u00d7').map((part) => Number(part.trim()));
    expect(Number.isFinite(width) && Number.isFinite(height)).toBe(true);
    // Asserted against the grabber's own cap rather than a number typed twice: the ask exists to
    // match it, so the two move together or this fails.
    expect(Math.max(width, height)).toBeGreaterThanOrEqual(GRAB_MAX_EDGE);

    // And the taller mode, not the wide one. Vertical field of view is what sets the ring count,
    // so a 16:9 frame plans a third more cells than a 4:3 one for the same sphere — measured,
    // 44 against 32. On a phone the 4:3 mode is the sensor's own and 16:9 is the crop of it, so
    // this asks for more of the picture rather than a differently shaped piece of it.
    expect(Math.max(width, height) / Math.min(width, height)).toBeCloseTo(4 / 3, 2);
  } finally {
    await server.close();
  }
});

test('a declined camera explains itself instead of failing silently', async ({ browser }) => {
  // The most common first-run outcome on a real phone, and the one where a bare error code
  // loses the user.
  const context = await browser.newContext({ permissions: [] });
  const page = await context.newPage();
  await page.addInitScript(() => {
    navigator.mediaDevices.getUserMedia = () => {
      const error = new Error('Permission denied');
      error.name = 'NotAllowedError';
      return Promise.reject(error);
    };
  });
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText(/permission/i, { timeout: 15000 });
    await expect(page.locator('#camera-state')).toHaveText('unavailable');
  } finally {
    await server.close();
    await context.close();
  }
});

test('a burst captures real pixels from the viewfinder', async ({ page }) => {
  // The whole pixel path in one assertion (ADR 0021): Chromium's fake camera produces frames, the
  // page draws one into a canvas and transfers the buffer, the worker holds it, and
  // BrowserCameraAccess copies it into the frame store on each peek. Every piece of that is
  // covered against a fake somewhere else; this is the only place they meet, and it is the only
  // check that would notice the port going back to refusing.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });

    // Through the client's own hook rather than the core directly: arming outside the capture
    // loop is the mistake ADR 0018 warned about, so the test must not be able to make it either.
    await aimAtACell(page);
    await viewfinderIsLive(page);
    const armed = await page.evaluate(() => window.sphanoramaCapture());
    expect(armed).toBe(true);

    // A settle of 150 ms and then five frames at 80 ms, so the burst needs the better part of
    // a second of ticks to fill.
    await expect(page.locator('#guidance')).toContainText(/captured|cell done/i, { timeout: 15000 });

    const candidates = await page.evaluate(async () => {
      const plan = await window.sphanoramaCore.captureSession.getPlan();
      for (const node of plan.value.nodes) {
        const got = await window.sphanoramaCore.captureSession.candidates(node.id);
        if (got.ok && got.value.length > 0) return got.value;
      }
      return [];
    });

    expect(candidates.length).toBe(5);
    // The frames are real: a grabbed frame carries the viewfinder's shape, and a burst that
    // allocated nothing would still have produced five candidates pointing at empty handles.
    expect(candidates[0].frame.width).toBeGreaterThan(0);
    expect(candidates[0].frame.height).toBeGreaterThan(0);
    // Distinct allocations, which is what the camera contract requires of repeated peeks and
    // what makes selecting a best frame from a burst mean anything.
    expect(new Set(candidates.map((c) => c.frame.id)).size).toBe(5);
  } finally {
    await server.close();
  }
});

test('a pick survives the tab that made it', async ({ page }) => {
  // The claim, end to end. A selection used to live in the review panel's own memory, so what the
  // strip showed as "in force" was whatever this tab had clicked — and a reload started again
  // from the ranking, silently disagreeing with the build, which reads the document. Everything
  // under this is covered against fakes; this is the only place a real document, a real reload
  // and a real strip meet.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await aimAtACell(page);
    await viewfinderIsLive(page);
    expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(true);
    await expect(page.locator('#guidance')).toContainText(/captured|cell done/i, { timeout: 15000 });

    const openTheStrip = async () => {
      await page.locator('#panel-toggle').click();
      const captured = page.locator('#coverage-map .cell[data-state="covered"]').first();
      await expect(captured).toBeVisible({ timeout: 15000 });
      await captured.click({ timeout: 5000 });
      await expect(page.locator('#strip-heading')).toContainText(/candidates, best first/i,
                                                                { timeout: 15000 });
    };
    const pressedIndex = async () => page.evaluate(() =>
      [...document.querySelectorAll('#strip button')]
        .findIndex((button) => button.getAttribute('aria-pressed') === 'true'));

    await openTheStrip();
    // The ranking's own pick is first, so choosing the last one is a choice that cannot be
    // confused with the default.
    expect(await pressedIndex()).toBe(0);
    const buttons = page.locator('#strip button');
    const last = (await buttons.count()) - 1;
    expect(last).toBeGreaterThan(0);
    await buttons.nth(last).click();
    await expect.poll(pressedIndex, { timeout: 15000 }).toBe(last);

    // Durability is eventual by design (ADR 0014) — the host coalesces writes behind a 250 ms
    // timer — and a reload does not wait for that timer. This is the same call `main.ts` makes
    // from its `pagehide` and `visibilitychange` handlers, which is how a real tab gets here;
    // driving it directly keeps the barrier deterministic instead of racing the timer, at the
    // cost of not covering that wiring. On a runner fast enough for the timer to have fired
    // already the assertion below survives without it, which is exactly why it stays: what it
    // rules out is a flake on a slower one, not a wrong answer on this one.
    await page.evaluate(() => window.sphanoramaHost.flush());
    await page.reload();
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#resume').click();
    await expect(page.locator('#stage')).toContainText('resumed', { timeout: 15000 });

    await openTheStrip();
    expect(await pressedIndex()).toBe(last);
  } finally {
    await server.close();
  }
});

test('the review strip shows the frames, not just their scores', async ({ page }) => {
  // The pixel path outward, end to end (ADR 0038). Everything inward is covered above; this is
  // the return leg — the store's bytes reduced by the preview engine, encoded by the generated
  // codec, back across the worker as a transferred buffer, and drawn onto a canvas by the review
  // client. Every piece has a test against a fake; this is the only place they meet, and the only
  // check that would notice a strip that went back to being a line of numbers.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await aimAtACell(page);
    await viewfinderIsLive(page);
    expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(true);
    await expect(page.locator('#guidance')).toContainText(/captured|cell done/i, {
      timeout: 15000,
    });

    // The panel folds away while a capture runs, so reviewing means asking for it back — which is
    // what a user does when the burst is in and they want to look at it.
    await page.locator('#panel-toggle').click();
    const captured = page.locator('#coverage-map .cell[data-state="covered"]').first();
    await expect(captured).toBeVisible({ timeout: 15000 });
    await captured.click({ timeout: 5000 });

    await expect(page.locator('#strip-heading')).toContainText(/candidates, best first/i, {
      timeout: 15000,
    });
    const thumbnails = page.locator('#strip canvas.thumb[data-preview="ready"]');
    await expect(thumbnails).toHaveCount(5, { timeout: 15000 });

    const drawn = await page.evaluate(() => Array.from(
      document.querySelectorAll('#strip canvas.thumb'),
      (canvas) => {
        const pixels = canvas.getContext('2d')
          .getImageData(0, 0, canvas.width, canvas.height).data;
        let lit = 0;
        let darkest = 255;
        let brightest = 0;
        for (let at = 0; at < pixels.length; at += 4) {
          if (pixels[at] !== 0 || pixels[at + 1] !== 0 || pixels[at + 2] !== 0) lit += 1;
          darkest = Math.min(darkest, pixels[at]);
          brightest = Math.max(brightest, pixels[at]);
        }
        return {
          width: canvas.width, height: canvas.height, lit, darkest, brightest,
          opaque: pixels[3] === 255,
        };
      }));

    expect(drawn).toHaveLength(5);
    for (const thumbnail of drawn) {
      // Reduced, and to the size the strip asked for rather than to the frame's own.
      expect(Math.max(thumbnail.width, thumbnail.height)).toBeLessThanOrEqual(PREVIEW_MAX_EDGE);
      expect(thumbnail.width).toBeGreaterThan(0);
      expect(thumbnail.lit).toBe(thumbnail.width * thumbnail.height);
      expect(thumbnail.opaque).toBe(true);
      // A photograph rather than a fill. Chromium's fake camera draws a moving pattern, so a
      // preview of it has range in it — which a canvas painted from zeros, or from one averaged
      // colour, would not.
      expect(thumbnail.brightest - thumbnail.darkest).toBeGreaterThan(8);
    }
  } finally {
    await server.close();
  }
});

test('a sphere from before the last capture cannot come back as this one', async ({ page }) => {
  // The whole tier generation, end to end, in the one place every piece of it meets: the token in
  // the OPFS index, the token in the session document, a reload between them, and the C ABI in
  // the middle. Unit tests cover each half against a fake; nothing else runs the real chain.
  //
  // The failure it is about does not look like one. Frame identities restart at 1 in every
  // session and the tier does not, so a second capture writes its own frames under the names the
  // first capture's document still carries — right identities, right size, really on disk. A
  // resume of the first project adopts them, pins them and builds a sphere out of somebody else's
  // pixels, with nothing failing anywhere (ADR 0035).
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });

    // A cell, so the session has frames in the tier and a document that names them. Cooling
    // spills a committed cell (ADR 0023), which is what puts them in the OPFS file at all.
    await aimAtACell(page);
    await viewfinderIsLive(page);
    expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(true);
    await expect(page.locator('#guidance')).toContainText(/captured|cell done/i, { timeout: 15000 });

    const first = await page.evaluate(async () => {
      const listed = await window.sphanoramaCore.project.list();
      // Durability is eventual by design (ADR 0014): ask for it rather than racing the timer.
      await window.sphanoramaHost.flush();
      return listed.ok && listed.value.length === 1 ? listed.value[0].id : null;
    });
    expect(first).not.toBeNull();

    await page.reload();
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });

    // Nothing on this fresh page has enabled anything, so the core has been told no camera and no
    // motion capability. Since ADR 0044 the sensor is what a resume establishes first, and this
    // is the only thing it can say — worth asserting rather than skipping past, because it is
    // also the answer a real device with no sensors gets.
    const beforeEnabling = await page.evaluate(
      (id) => window.sphanoramaCore.captureSession.resume(id), first);
    expect(beforeEnabling.ok).toBe(false);
    expect(beforeEnabling.status.code).toBe('SensorUnavailable');

    // Then the same tier, through the offer the page makes, which starts motion and opens a
    // camera on the way. It succeeds — and a success is a stronger statement than the refusal
    // this used to assert, because every stage had to pass to reach it: the document was written
    // (so the tier answered when it was checkpointed), it parsed, its token matched the one the
    // index came back with, and the store took back every frame it names.
    await page.locator('#resume').click();
    await expect(page.locator('#stage')).toContainText('resumed', { timeout: 15000 });

    // And now a different sphere is started on this device, which empties the tier (ADR 0034) and
    // fills it again from identity 1. Through a second reload and the enable button rather than
    // through the core: `enable` hides itself once it has run, and a `begin` driven straight
    // through the facade would have to open a camera the resume above is already holding.
    await page.reload();
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });

    const afterAnotherCapture = await page.evaluate(async (id) => {
      // Ended first, or the refusal below would be the one about a session already being in
      // progress — the same status code for an entirely different reason. Ending closes the
      // camera, which costs nothing here: the tier is checked long before one is opened.
      await window.sphanoramaCore.captureSession.end();
      return window.sphanoramaCore.captureSession.resume(id);
    }, first);
    expect(afterAnotherCapture.ok).toBe(false);
    expect(afterAnotherCapture.status.code).toBe('FailedPrecondition');
    expect(afterAnotherCapture.status.detail).toContain('spill tier');
  } finally {
    await server.close();
  }
});

test('asks iOS for motion before it goes anywhere near the camera', async ({ browser }) => {
  // Reported from an iPhone: `motion unavailable`, in every orientation, forever. iOS grants
  // DeviceOrientationEvent only during a transient user activation, and awaiting a permission
  // prompt spends it — so a motion request made after `getUserMedia` is refused unread, and the
  // phone can never aim itself. The adapter was already careful not to spend the activation
  // between its own two requests; the activation was gone before it was called at all.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    window.__asked = [];
    // An iPhone has no Generic Sensor API, which is the whole reason it reaches the gated
    // orientation event at all. Leaving Chromium's in place would take the ungated path and test
    // nothing about iOS.
    delete window.AbsoluteOrientationSensor;
    const media = navigator.mediaDevices;
    const real = media.getUserMedia.bind(media);
    media.getUserMedia = (constraints) => {
      window.__asked.push('camera');
      return real(constraints);
    };
    // The iOS gate, which Chromium does not have. Recording when it is *called* rather than when
    // it resolves is the whole point: what iOS checks is whether the gesture was still live at
    // the moment of the call.
    window.DeviceOrientationEvent.requestPermission = async () => {
      window.__asked.push('motion');
      return 'granted';
    };
    window.DeviceMotionEvent.requestPermission = async () => {
      window.__asked.push('rates');
      return 'granted';
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });

    const asked = await page.evaluate(() => window.__asked);
    expect(asked).toContain('motion');
    expect(asked.indexOf('motion')).toBeLessThan(asked.indexOf('camera'));
  } finally {
    await server.close();
    await context.close();
  }
});

test('says why motion is unavailable rather than only that it is', async ({ browser }) => {
  // One word for every cause is what made the iPhone reading unreadable: a declined grant, a
  // gesture that had expired and a device with no sensors all printed `unavailable`, and the
  // status that told them apart was thrown away at this line. The `locks` row learned this
  // lesson already (ADR 0022).
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    // No Generic Sensor API, and a gate that rejects rather than declines — which is what iOS
    // does when the call did not follow a user gesture. `start` fails outright here, and that is
    // the path that had only one word for every cause.
    delete window.AbsoluteOrientationSensor;
    window.DeviceOrientationEvent.requestPermission = async () => {
      throw new Error('requestPermission requires a user gesture');
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();

    await expect(page.locator('#motion-state')).toContainText(/gesture/i, { timeout: 15000 });
  } finally {
    await server.close();
    await context.close();
  }
});

test('a burst locks the camera when the camera can be locked', async ({ browser }) => {
  // Chromium's fake device lists `manual` for exposure and focus and no whiteBalanceMode at all,
  // and it *starts* in manual — so it grants those two before anything is asked. This patches in
  // a track that grants all three and, more to the point, honours the request rather than having
  // been there already: locks asked for, confirmed by reading the settings back, and the burst
  // armed holding them (ADR 0022).
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    // A camera that can lock, and honours the request. Patched on the prototype so it applies to
    // whatever track getUserMedia hands back.
    const modes = ['continuous', 'manual'];
    let settled = {};
    const settings = MediaStreamTrack.prototype.getSettings;
    MediaStreamTrack.prototype.getCapabilities = function () {
      return { exposureMode: modes, whiteBalanceMode: modes, focusMode: modes };
    };
    // The three modes are stated rather than passed through, and it is load-bearing: Chromium's
    // fake device reports `exposureMode: 'manual'` from the moment it opens, so a fake that let
    // the real settings show through has `setLocks` find every lock already held and apply no
    // constraint at all — and the delay below, which is what makes this test's window, never runs.
    // The camera has to start out adapting for pinning it to be an event.
    MediaStreamTrack.prototype.getSettings = function () {
      const adapting = {
        exposureMode: 'continuous', whiteBalanceMode: 'continuous', focusMode: 'continuous',
      };
      return { ...settings.call(this), ...adapting, ...settled };
    };
    MediaStreamTrack.prototype.applyConstraints = async function (constraints) {
      // Merged, the way a track's settings actually behave: each lock is negotiated in a set of
      // its own now, so replacing wholesale would leave only whichever went last.
      for (const asked of constraints?.advanced ?? []) settled = { ...settled, ...asked };
    };
    window.__locksApplied = () => settled;
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await aimAtACell(page);
    await viewfinderIsLive(page);

    expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(true);
    await expect(page.locator('#guidance')).toContainText(/captured|cell done/i, { timeout: 15000 });

    // Arming took the locks. If the client had asked for them without confirming, or the core
    // had refused them, the burst above would not have completed at all.
    expect(await page.evaluate(() => window.__locksApplied())).toMatchObject({
      exposureMode: 'continuous',   // released again once the burst committed
    });

    // And the cell captured with them, which is the point: five candidates a selection engine
    // can compare on sharpness because they share an exposure.
    const count = await page.evaluate(async () => {
      const plan = await window.sphanoramaCore.captureSession.getPlan();
      for (const node of plan.value.nodes) {
        const got = await window.sphanoramaCore.captureSession.candidates(node.id);
        if (got.ok && got.value.length > 0) return got.value.length;
      }
      return 0;
    });
    expect(count).toBe(5);
  } finally {
    await server.close();
    await context.close();
  }
});

test('a camera that dies while the page is still enabling does not start a capture', async ({ browser }) => {
  // `opened.ok` is a fact about a call that has already returned. Two awaits sit between it and
  // the decision to begin — the motion capability and the sensor start — and a track that ends
  // inside that window used to leave the page announcing "capturing — 32 cells planned", panel
  // folded and shutter enabled, with `#camera-state` saying the camera had been taken away, all
  // on screen at once.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    // Handed back already dead, which is the harder half of the same case: the `ended` event has
    // fired before anything could listen for it, so only asking the track its `readyState` finds
    // this. `stop()` puts a track into `ended` synchronously.
    const real = navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);
    navigator.mediaDevices.getUserMedia = async (...args) => {
      const stream = await real(...args);
      for (const track of stream.getTracks()) track.stop();
      return stream;
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();

    await expect(page.locator('#stage')).toContainText(/taken away/i, { timeout: 15000 });
    // And no session behind it. `capturing` is what `beginSession` writes, and it is the word the
    // page had no business saying.
    await expect(page.locator('#stage')).not.toContainText('capturing');
    // And no session behind it, asserted against the core rather than against the page. This used
    // to read the shutter's `disabled` attribute, and the obvious replacement — that
    // `window.sphanoramaCapture()` answers false — is satisfied by a default: it returns false on
    // a page that has done nothing at all, because the hook has no target cell to arm until a
    // plan exists. It would have passed if `enable` had never run.
    //
    // `getPlan` refuses unless a session is active, which is the fact this test is about — and it
    // is worth being exact, because the obvious stronger claim is false: this does not prove no
    // session was ever begun, only that none is open now. What makes it enough here is that
    // `enable` is the only path that begins one and it never reached `beginSession`.
    const planned = await page.evaluate(async () => {
      const got = await window.sphanoramaCore.captureSession.getPlan();
      return got.ok ? got.value.nodes.length : -1;
    });
    expect(planned).toBe(-1);
  } finally {
    await server.close();
    await context.close();
  }
});

test('a held cell fires one burst, not one per tick', async ({ browser }) => {
  // What is left of `the shutter stays taken from the press until the burst is over` once the
  // shutter is gone (ADR 0044). That test watched the button for a window in which a second press
  // would have been accepted; the window it was about is still there, and now nothing but the
  // core closes it — the dwell's counter restarts when it fires and needs a whole two seconds
  // again, `armAt` refuses a second arm while one is in flight, and a burst in flight overwrites
  // the action guidance reports, so the dwell resets rather than continuing to mature underneath
  // it.
  //
  // Three guards for one property, and they are not independent — which is worth writing down,
  // because the obvious sentence here ("break any one and the phone re-arms") is false and was
  // measured to be, twice.
  //
  // Neither of the two core-side guards is what this test measures. Removing the counter's
  // restart changes nothing here, and neither does letting the dwell keep serving while a burst
  // is in flight: this runner's burst finishes in about 1.4 seconds and the dwell needs two, so
  // the second `Fire` never arrives before the cell is captured and the question stops being
  // asked. Both are pinned natively instead, where a burst can be paced long enough to outlast a
  // dwell — `AFireNobodyActedOnComesRoundAgainWhileTheCellIsStillHeld` and
  // `ABurstInFlightDoesNotServeTheDwellThatFiredIt`.
  //
  // What this one isolates is the coverage rule: making `Locate` answer `HoldStill` on a cell it
  // has already captured fires a second burst into it, and the count below comes back 8 — the
  // per-cell cap (ADR 0037), which is what ten frames become.
  //
  // The camera is slowed on purpose, as it was here before: on one that takes the locks instantly
  // the window is a few frames and the test would be hoping to land in it rather than opening it.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    const modes = ['continuous', 'manual'];
    let settled = {};
    const settings = MediaStreamTrack.prototype.getSettings;
    MediaStreamTrack.prototype.getCapabilities = function () {
      return { exposureMode: modes, whiteBalanceMode: modes, focusMode: modes };
    };
    // The three modes are stated rather than passed through, and it is load-bearing: Chromium's
    // fake device reports `exposureMode: 'manual'` from the moment it opens, so a fake that let
    // the real settings show through has `setLocks` find every lock already held and apply no
    // constraint at all — and the delay below, which is what makes this test's window, never runs.
    // The camera has to start out adapting for pinning it to be an event.
    MediaStreamTrack.prototype.getSettings = function () {
      const adapting = {
        exposureMode: 'continuous', whiteBalanceMode: 'continuous', focusMode: 'continuous',
      };
      return { ...settings.call(this), ...adapting, ...settled };
    };
    // 300 ms per constraint set, three of them: about a second of arming, well inside the three
    // the page allows one write, and long enough that a dwell left running underneath it would
    // mature again many frames before the burst ends.
    MediaStreamTrack.prototype.applyConstraints = function (constraints) {
      return new Promise((resolve) => setTimeout(() => {
        for (const asked of constraints?.advanced ?? []) settled = { ...settled, ...asked };
        resolve();
      }, 300));
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    const aim = await aimAtACell(page);
    await viewfinderIsLive(page);

    // Held, without pause, right through the burst the hold starts and well past the end of it.
    // The hold has to continue while the burst runs: the dwell only advances on ticks a sample
    // arrived on, so a test that stopped dispatching would be watching a dwell that had stopped
    // for a reason of its own.
    let sawFiring = false;
    for (let i = 0; i < 200; i += 1) {
      await page.evaluate(({ a, b, g }) => {
        window.dispatchEvent(new DeviceOrientationEvent('deviceorientation',
          { alpha: a, beta: b, gamma: g }));
      }, { a: aim.alpha, b: aim.beta, g: aim.gamma });
      await page.waitForTimeout(50);
      const text = await page.locator('#guidance').textContent();
      if (/capturing/i.test(text)) sawFiring = true;
      if (sawFiring && /already captured/i.test(text)) break;
    }
    expect(sawFiring, 'the hold never fired a burst at all').toBe(true);

    // One burst, and this is the whole assertion: five candidates in the cell that was held, not
    // ten. Read from the cell by name rather than summed, so a second burst that landed somewhere
    // else would fail the count below instead of hiding in this one.
    const inCell = await page.evaluate(async (id) => {
      const got = await window.sphanoramaCore.captureSession.candidates(id);
      return got.ok ? got.value.length : -1;
    }, aim.id);
    expect(inCell).toBe(5);
    expect(await countCandidates(page)).toBe(5);
  } finally {
    await server.close();
    await context.close();
  }
});

test('every camera capability the core reads crosses the seam it reads it through', async ({ page }) => {
  // The seam nothing was holding. `host_camera_metric` and `capture-host.ts` agree by an integer
  // index and a property name, and neither was checked by anything: `maxBurstFps` was in
  // `CameraCapabilities` for the life of the field, had no `case` in that switch, and read as
  // zero — which the manager is right to treat as "the platform will not say", so a floor that was
  // never wired looked exactly like a browser declining to answer. Nothing failed.
  //
  // Every test written when the field was finally wired sat on the TypeScript side of the
  // boundary, so renaming `case 8` to `case 9` left the native suite, vitest and the browser suite
  // all green with the floor dead again. This is the assertion that fails instead — the only one
  // in the tree that runs `BrowserCameraAccess::Open`, which executes under wasm and nowhere else.
  //
  // Chromium's `--use-fake-device-for-media-stream` reports a real resolution and a real frame
  // rate, so the values are the device's rather than a fixture's, and the assertions are about
  // *shape and plausibility* rather than exact numbers a runner is entitled to change.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await viewfinderIsLive(page);

    const seen = await page.evaluate(async () => {
      const got = await window.sphanoramaCore.captureSession.cameraInUse();
      return got.ok ? got.value : { error: got.status };
    });

    expect(seen.error, `the core would not say what camera it has: ${JSON.stringify(seen.error)}`)
      .toBeUndefined();

    // **Against what the page itself sees, field by field, rather than against `> 0`.**
    //
    // The first version of this asserted presence — every number greater than zero, every boolean
    // a boolean — and a reviewer showed what that buys: renumbering `case 4`, `5` or `7` left it
    // green, because `typeof x === 'boolean'` is satisfied by the codec rather than by the switch,
    // and swapping `case 0` with `case 1` or `case 2` with `case 3` left it green too, because a
    // transposed pair is still two positive numbers. A transposed field of view sizes the
    // tessellation from the wrong axis.
    //
    // So the assertion is identity: the page reads the same track through its own adapter, and
    // every field the core reports has to equal what the page sees. That is the seam's whole
    // claim, and it catches any transposition and any renumbering — of seven of the eight cases.
    //
    // **`supportsTorch` is the eighth and is unobservable here**, measured by renumbering each case
    // in turn: Chromium's fake camera reports no torch, so the page says `false`, and a core that
    // never reads the metric says `false` too. Equality cannot separate them. It is not a hole in
    // the assertion but in the runner — on a device with a torch this catches it like the rest —
    // and it is written down because "the seam is pinned" was the claim a reviewer disproved once
    // already, and half-pinned is what it actually is.
    const pageSees = await page.evaluate(() => window.sphanoramaCameraCapabilities());
    expect(seen.maxWidth, 'maxWidth').toBe(pageSees.maxWidth);
    expect(seen.maxHeight, 'maxHeight').toBe(pageSees.maxHeight);
    expect(seen.maxBurstFps, 'maxBurstFps').toBe(pageSees.maxBurstFps);
    expect(seen.supportsTorch, 'supportsTorch').toBe(pageSees.supportsTorch);
    expect(seen.supportsExposureLock, 'supportsExposureLock').toBe(pageSees.supportsExposureLock);
    expect(seen.supportsFocusLock, 'supportsFocusLock').toBe(pageSees.supportsFocusLock);

    // The field of view is derived in the host from the resolution rather than read from a metric,
    // so equality against the page would compare a constant with itself. What it can say is that
    // the pair is oriented the way the frame is — a landscape frame has the wider angle across —
    // which is the transposition this file's own tessellation depends on.
    expect(seen.horizontalFovDeg, 'horizontalFovDeg').toBeGreaterThan(0);
    expect(seen.verticalFovDeg, 'verticalFovDeg').toBeGreaterThan(0);
    expect(seen.maxWidth > seen.maxHeight
      ? seen.horizontalFovDeg > seen.verticalFovDeg
      : seen.verticalFovDeg > seen.horizontalFovDeg,
    `the field of view is transposed against the frame: ${seen.maxWidth}x${seen.maxHeight} `
    + `reported as ${seen.horizontalFovDeg}x${seen.verticalFovDeg}`).toBe(true);
  } finally {
    await server.close();
  }
});

test('the camera the core paces a burst by is the one the locks left behind', async ({ browser }) => {
  // ADR 0045 end to end, and the three pieces of it that nothing else holds.
  //
  // The decision is that `CaptureSessionManager::ArmBurst` re-asks its camera port after applying
  // the locks, because applying them is what changes the answer: pinning an exposure long is what
  // drops a camera from 30 fps to 15, and `maxBurstFps` is the floor the manager puts under a
  // burst's interval and settle (ADR 0018, ADR 0032). A burst paced at 30 on a camera making 15
  // fills with duplicates of one exposure, and selection then ranks a frame against copies of
  // itself.
  //
  // Three things have to be true for that to work, and a reviewer showed each could be deleted
  // with every suite green:
  //
  //   * the page pushes the live capability set after the lock write settles (`main.ts`), because
  //     the port is resident (ADR 0014) — it answers from the worker's cache, and a pull with
  //     nothing pushing behind it reads what `open` said;
  //   * `BrowserCameraAccess::Capabilities()` reads the host's metrics rather than returning an
  //     empty struct — the contract suite runs a fake and cannot see this one at all;
  //   * `ArmBurst` actually calls it.
  //
  // Deleting any one of the three leaves the core pacing by 30. The camera below is made to slow
  // when its exposure is pinned, so 30 and 15 are the two answers and only the right wiring
  // produces the second.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    const modes = ['continuous', 'manual'];
    let settled = {};
    const settings = MediaStreamTrack.prototype.getSettings;
    MediaStreamTrack.prototype.getCapabilities = function () {
      return { exposureMode: modes, whiteBalanceMode: modes, focusMode: modes };
    };
    MediaStreamTrack.prototype.getSettings = function () {
      // The three modes are stated rather than passed through, and that is not tidiness: Chromium's
      // fake device reports `exposureMode: 'manual'` from the moment it opens, so a fake that let
      // the real settings show through would have `setLocks` find the lock already held, apply no
      // constraint at all, and change nothing. The camera has to start out adapting for pinning it
      // to be an event.
      const adapting = { exposureMode: 'continuous', whiteBalanceMode: 'continuous', focusMode: 'continuous' };
      const now = { ...settings.call(this), ...adapting, ...settled };
      // The whole point of the fake: a pinned exposure costs frame rate. Real cameras do this and
      // it is why the re-ask exists.
      return { ...now, frameRate: now.exposureMode === 'manual' ? 15 : 30 };
    };
    MediaStreamTrack.prototype.applyConstraints = function (constraints) {
      for (const asked of constraints?.advanced ?? []) settled = { ...settled, ...asked };
      return Promise.resolve();
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await viewfinderIsLive(page);

    const rateInCore = () => page.evaluate(async () => {
      const got = await window.sphanoramaCore.captureSession.cameraInUse();
      return got.ok ? got.value.maxBurstFps : null;
    });

    // What `open` reported, which is the number the core would keep for ever without the push.
    expect(await rateInCore(), 'the free-running rate never reached the core').toBe(30);

    await aimAtACell(page);
    const armed = await page.evaluate(() => window.sphanoramaCapture());
    // No arm, no `SetLocks`, no re-ask — and the assertion below would then be about nothing.
    expect(armed, 'nothing was armed, so no lock was ever applied').toBe(true);

    expect(await rateInCore(),
      'the core is pacing this burst by the camera it had before it pinned the exposure')
      .toBe(15);
  } finally {
    await server.close();
    await context.close();
  }
});

test('a second arm while the first is still crossing the worker says so', async ({ page }) => {
  // The one exit from `armAt` that used to report nothing, and the dwell's retry is what made it
  // reachable: a `Fire` the core re-offers two seconds later lands while the first arm is still in
  // flight — `armOnce` waits on a lock write bounded at three seconds — and the ring has restarted
  // from zero meanwhile, because the core resets its counter when it fires. So the user watched
  // the ring fill, saw nothing happen, and watched it fill again.
  //
  // Round 6 turned that silence into a line. A reviewer then pointed out that the line was
  // asserted nowhere: the string appears exactly once in the tree, in `main.ts`, and `main.ts` has
  // no unit test of its own — so deleting it, or letting a later reorder put the guidance line
  // after it, would leave every test green. This is the assertion.
  //
  // Driven by calling the hook twice without awaiting the first, which is deterministic rather
  // than a race: `armAt` sets `arming` synchronously, before any await, so the second call is
  // already refused by the time it can yield.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await aimAtACell(page);
    await viewfinderIsLive(page);

    const { first, second, said } = await page.evaluate(async () => {
      const armed = window.sphanoramaCapture();
      const refused = await window.sphanoramaCapture();
      // Read before awaiting the first, which finishes by painting a line of its own over this.
      const line = document.querySelector('#guidance').textContent;
      return { first: await armed, second: refused, said: line };
    });

    // The first arm has to have been a real one. Since ADR 0043 the loop arms on the core's own
    // `Fire`, so a run where the dwell had already matured during `aimAtACell` would send *both*
    // of these calls down the `arming` branch — `second` false, the line still right, and the test
    // green without ever having exercised the thing it is named for.
    expect(first, 'the first call was refused too, so nothing was ever in flight to collide with')
      .toBe(true);
    expect(second, 'a second arm was accepted while one was in flight').toBe(false);
    expect(said, 'the refused arm said nothing, so the user saw the ring restart with no reason')
      .toMatch(/still arming that cell/i);
  } finally {
    await server.close();
  }
});

test('a session ended mid-burst still says which locks that burst had', async ({ browser }) => {
  // `#locks` reads "no burst has run yet" from an empty `lastLocksLine`, and `onCloseCamera` writes
  // that same empty string to mean "the record was discarded". Two writers, one reading — and
  // `End()` runs them in the order that collides: it disarms first, which queues the release, and
  // closes second, which clears the line before the release resolves and paints. The row then
  // reports "no burst has run yet" about a burst whose locks it had just given back.
  //
  // The camera is slowed so a burst is genuinely in flight when the session ends, which is the
  // arrangement the collision needs; on an instant camera the burst is over before `end()` lands.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    const modes = ['continuous', 'manual'];
    let settled = {};
    const settings = MediaStreamTrack.prototype.getSettings;
    MediaStreamTrack.prototype.getCapabilities = function () {
      return { exposureMode: modes, whiteBalanceMode: modes, focusMode: modes };
    };
    // The three modes are stated rather than passed through, and it is load-bearing: Chromium's
    // fake device reports `exposureMode: 'manual'` from the moment it opens, so a fake that let
    // the real settings show through has `setLocks` find every lock already held and apply no
    // constraint at all — and the delay below, which is what makes this test's window, never runs.
    // The camera has to start out adapting for pinning it to be an event.
    MediaStreamTrack.prototype.getSettings = function () {
      const adapting = {
        exposureMode: 'continuous', whiteBalanceMode: 'continuous', focusMode: 'continuous',
      };
      return { ...settings.call(this), ...adapting, ...settled };
    };
    MediaStreamTrack.prototype.applyConstraints = function (constraints) {
      return new Promise((resolve) => setTimeout(() => {
        for (const asked of constraints?.advanced ?? []) settled = { ...settled, ...asked };
        resolve();
      }, 300));
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await aimAtACell(page);
    await viewfinderIsLive(page);

    // Fired and not awaited, so the session can be ended while the burst is still filling.
    await page.evaluate(() => { window.__capturing = window.sphanoramaCapture(); });
    await expect(page.locator('#locks')).not.toHaveText('—', { timeout: 15000 });
    const held = await page.locator('#locks').textContent();
    expect(held).toMatch(/exposure|focus|white balance|does not report/i);

    await page.evaluate(async () => { await window.sphanoramaCore.captureSession.end(); });

    // Waited for the release to actually land before judging the row, because the clear happens
    // *while* the release is in flight: for the first second the row still shows the arm's line and
    // a negative assertion passes against the defect. Measured under sabotage as the arm line at
    // +300 ms and "no burst has run yet" from +900 ms on.
    await expect(page.locator('#locks')).toContainText('released', { timeout: 15000 });
    // And now: whatever else it says, it must not claim nothing has run. "no burst has run yet" is
    // the row forgetting a burst it had already described a second earlier.
    await expect(page.locator('#locks')).not.toContainText('no burst has run yet');
    await expect(page.locator('#locks')).toContainText(held.split(' · ')[0]);
  } finally {
    await server.close();
    await context.close();
  }
});

test('a camera taken away mid-session takes the capture loop with it', async ({ page }) => {
  // A `<video>` keeps `readyState 4` and its dimensions after its track ends, so both of the
  // frame grabber's guards pass and every grab returns a copy of the last frame the camera
  // produced. A burst armed after the camera was taken banked five of them: sharp, well scored,
  // all of the same instant, filed under a cell. That is ADR 0041's wrong pixels reached from the
  // other end, and it is undetectable downstream in exactly the same way.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await aimAtACell(page);
    await viewfinderIsLive(page);

    await page.evaluate(() => {
      const stream = document.querySelector('video').srcObject;
      for (const track of stream.getTracks()) track.dispatchEvent(new Event('ended'));
    });
    await expect(page.locator('#camera-state')).toHaveText('taken away', { timeout: 15000 });

    // Refused, through the same path the button takes.
    expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(false);
    // And nothing banked. This is the assertion the finding is about: before it, this came back 5.
    // Late, for the same reason as the lock-timeout test above: a burst banks its frames over the
    // half second after it is armed, so a count taken straight away agrees with an armed burst.
    await page.waitForTimeout(4000);
    expect(await countCandidates(page)).toBe(0);
    await expect(page.locator('#stage')).toContainText(/taken away/i);
  } finally {
    await server.close();
  }
});

test('a slow camera spends its lock budget on the track, not in the queue', async ({ browser }) => {
  // The 3 s bound on a lock write used to start when the write joined the chain rather than when
  // it reached the track, and every refusal queued a release behind the write it had given up on
  // — so the queue grew by one write per failure. On a camera taking 700 ms per constraint the
  // first burst succeeded and every burst after it was refused at 3 s intervals, most of that
  // spent waiting, while the track answered fourteen constraints back to back without an idle
  // moment. One cell per session, blamed on a camera that was answering everything it was asked.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    const modes = ['continuous', 'manual'];
    let settled = {};
    const settings = MediaStreamTrack.prototype.getSettings;
    MediaStreamTrack.prototype.getCapabilities = function () {
      return { exposureMode: modes, whiteBalanceMode: modes, focusMode: modes };
    };
    // The three modes are stated rather than passed through, and it is load-bearing: Chromium's
    // fake device reports `exposureMode: 'manual'` from the moment it opens, so a fake that let
    // the real settings show through has `setLocks` find every lock already held and apply no
    // constraint at all — and the delay below, which is what makes this test's window, never runs.
    // The camera has to start out adapting for pinning it to be an event.
    MediaStreamTrack.prototype.getSettings = function () {
      const adapting = {
        exposureMode: 'continuous', whiteBalanceMode: 'continuous', focusMode: 'continuous',
      };
      return { ...settings.call(this), ...adapting, ...settled };
    };
    // Slow, and never silent: 700 ms per constraint set, which is a 6x outlier over the 120 ms
    // this codebase has measured and well inside the 3 s the page allows one write.
    MediaStreamTrack.prototype.applyConstraints = function (constraints) {
      return new Promise((resolve) => setTimeout(() => {
        for (const asked of constraints?.advanced ?? []) settled = { ...settled, ...asked };
        resolve();
      }, 700));
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await aimAtACell(page);
    await viewfinderIsLive(page);

    // Three in a row, because the failure is cumulative: the first one always worked and it was
    // the second and third that were refused by a queue the first one had lengthened.
    for (let attempt = 0; attempt < 3; attempt += 1) {
      expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(true);
      await expect(page.locator('#guidance'))
        .toContainText(/captured|cell done/i, { timeout: 30000 });
      await aimAtACell(page);
    }
    await expect(page.locator('#locks')).not.toContainText(/did not answer/i);
  } finally {
    await server.close();
    await context.close();
  }
});

test('a camera taken away mid-arm does not arm anything', async ({ browser }) => {
  // The guard at the top of `armOnce` is a fact about the moment the arm started, and there is an
  // await between it and the arming. A camera taken away in that window still answers
  // `camera.setLocks`: the adapter's stream is closed only by its own `close()`, which the page
  // never calls, and `applyConstraints` on an ended track rejects into a catch written to swallow
  // exactly that — so the write comes back *answered* and every line after it proceeded.
  //
  // What that armed is a burst the manager holds as firing for the life of the tab, over a loop
  // that had already stopped, with `#locks` rewritten to describe a camera that was gone and
  // `#guidance` saying "capturing without … lock" under a stage line saying to reload.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    const modes = ['continuous', 'manual'];
    let settled = {};
    const settings = MediaStreamTrack.prototype.getSettings;
    MediaStreamTrack.prototype.getCapabilities = function () {
      return { exposureMode: modes, whiteBalanceMode: modes, focusMode: modes };
    };
    // The three modes are stated rather than passed through, and it is load-bearing: Chromium's
    // fake device reports `exposureMode: 'manual'` from the moment it opens, so a fake that let
    // the real settings show through has `setLocks` find every lock already held and apply no
    // constraint at all — and the delay below, which is what makes this test's window, never runs.
    // The camera has to start out adapting for pinning it to be an event.
    MediaStreamTrack.prototype.getSettings = function () {
      const adapting = {
        exposureMode: 'continuous', whiteBalanceMode: 'continuous', focusMode: 'continuous',
      };
      return { ...settings.call(this), ...adapting, ...settled };
    };
    // 300 ms per constraint opens the window by construction rather than by luck — the same
    // arrangement the shutter test needs, and for the same reason.
    MediaStreamTrack.prototype.applyConstraints = function (constraints) {
      return new Promise((resolve) => setTimeout(() => {
        for (const asked of constraints?.advanced ?? []) settled = { ...settled, ...asked };
        resolve();
      }, 300));
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await aimAtACell(page);
    await viewfinderIsLive(page);

    // Pressed, then the lens is taken 100 ms later — inside the second the lock writes take.
    const armed = await page.evaluate(async () => {
      const capturing = window.sphanoramaCapture();
      await new Promise((resolve) => setTimeout(resolve, 100));
      const stream = document.querySelector('video').srcObject;
      for (const track of stream.getTracks()) track.dispatchEvent(new Event('ended'));
      return capturing;
    });
    expect(armed).toBe(false);

    await expect(page.locator('#stage')).toContainText(/taken away/i, { timeout: 15000 });
    // Nothing banked, counted late enough that a burst armed after the camera went would have
    // filled by now.
    await page.waitForTimeout(4000);
    expect(await countCandidates(page)).toBe(0);
    // And the page is still saying the camera is gone rather than reporting a capture over it.
    await expect(page.locator('#stage')).toContainText(/taken away/i);
    // Including the locks row, which the refusal's own release paints a second later. It must not
    // end at "no burst has run yet" — the camera really did take and give back three locks, and
    // that string is the row claiming nothing ever ran.
    await expect(page.locator('#locks')).not.toContainText('no burst has run yet');
    await expect(page.locator('#locks')).toContainText(/taken away/i);
  } finally {
    await server.close();
    await context.close();
  }
});

test('an arm still in flight does not hand back a camera the core has closed', async ({ browser }) => {
  // ADR 0045's push, landing after the thing it describes is gone.
  //
  // `armOnce` pushes two facts into the worker on its way to arming — the locks the camera
  // confirmed, and the capability set read live off the track — and both sit behind an
  // `await writeLocks(...)` that takes as long as `applyConstraints` takes. `End()` inside that
  // window runs `closeCamera` in the worker, which sets the host's camera to null, and then the
  // page's `onCloseCamera` stops the tracks. The arm then wakes up and pushes anyway: the host
  // takes the struct, `cameraOpen()` reads true again, and the core believes it has a camera.
  //
  // What that costs is the next session. `Begin` refuses with CameraUnavailable when the page has
  // no camera open, and that refusal is the whole reason the page and the core cannot disagree
  // about whether a capture is possible. With a stale push standing in for one, `Begin` succeeded
  // and planned a full tessellation — against `maxWidth 0, maxHeight 0`, because the struct was
  // read off a track that had already ended, so the field of view came from the host's assumed
  // fallback rather than from a lens.
  //
  // `cameraLost()` is not the guard for this and could not have been: it reads `cameraTakenAway`,
  // which only the `ended` listener writes, and `track.stop()` — how the core's own close ends a
  // track — fires no `ended`. `cameraHeld()` asks the tracks, so it is true of every way a camera
  // goes away, an orderly End included.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    const modes = ['continuous', 'manual'];
    let settled = {};
    const settings = MediaStreamTrack.prototype.getSettings;
    MediaStreamTrack.prototype.getCapabilities = function () {
      return { exposureMode: modes, whiteBalanceMode: modes, focusMode: modes };
    };
    // The three modes are stated rather than passed through, and it is load-bearing: Chromium's
    // fake device reports `exposureMode: 'manual'` from the moment it opens, so a fake that let
    // the real settings show through has `setLocks` find every lock already held and apply no
    // constraint at all — and the delay below, which is what makes this test's window, never runs.
    // The camera has to start out adapting for pinning it to be an event.
    MediaStreamTrack.prototype.getSettings = function () {
      const adapting = {
        exposureMode: 'continuous', whiteBalanceMode: 'continuous', focusMode: 'continuous',
      };
      return { ...settings.call(this), ...adapting, ...settled };
    };
    // 600 ms a constraint, three constraints: 1.8 s of window, comfortably inside `writeLocks`'
    // own three-second bound. Slower than that and the write is abandoned instead, which takes a
    // different branch out of `armOnce` and would make this test pass without ever reaching the
    // pushes it is about.
    MediaStreamTrack.prototype.applyConstraints = function (constraints) {
      return new Promise((resolve) => setTimeout(() => {
        for (const asked of constraints?.advanced ?? []) settled = { ...settled, ...asked };
        resolve();
      }, 600));
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await aimAtACell(page);
    await viewfinderIsLive(page);

    // Armed, then the session ended 200 ms later — while the lock write is still parked.
    const armed = await page.evaluate(async () => {
      const capturing = window.sphanoramaCapture();
      await new Promise((resolve) => setTimeout(resolve, 200));
      await window.sphanoramaCore.captureSession.end();
      return capturing;
    });
    // The arm has to have been a real one in a real window. `false` here is what the fix produces
    // *and* what an arm refused before it ever started produces, so the assertions below are what
    // carry the test; this one only says the arm did not somehow succeed over a closed camera.
    expect(armed, 'a burst armed over a camera the core had closed').toBe(false);

    // The fact the whole thing turns on: with the camera closed, the core must not think it has
    // one. Asked through `Begin`, because that is the caller whose refusal matters — a session
    // that starts here is one that plans a sphere against a lens nobody measured.
    const begun = await page.evaluate(async () => {
      const core = window.sphanoramaCore;
      const project = await core.project.create('a second capture, after the first was ended');
      const started = await core.captureSession.begin(project.value, {
        strategy: 'Rings', horizontalFovDeg: 0, verticalFovDeg: 0, overlapTarget: 0.3,
        acceptanceConeDeg: 4, coverPoles: true, motion: 'None',
      });
      if (!started.ok) return { code: started.status.code };
      const plan = await core.captureSession.getPlan();
      return { code: null, cells: plan.ok ? plan.value.nodes.length : -1 };
    });
    expect(begun.code,
      `the core still had a camera and planned ${begun.cells} cells over a closed one`)
      .toBe('CameraUnavailable');
  } finally {
    await server.close();
    await context.close();
  }
});

test('a camera taken away while the arm is in the core says so, and keeps saying it', async ({ browser }) => {
  // The window after `armBurst` is sent and before it answers. The check before it covers the lock
  // write; this covers the round trip after it, and the reason it matters is not the arm — there is
  // no page route to `Disarm`, so a burst armed here stays armed whatever anything returns — but
  // the *message*. Everything `armOnce` says after this point goes to `#guidance` through
  // `sayForAWhile`, which writes it directly, and the loop has already taken its terminal return:
  // nothing will ever overwrite it. "capturing without focus lock", or an arming refusal telling
  // the user to re-aim, becomes the page's last word for the life of the tab, under a stage line
  // telling them to reload.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    const modes = ['continuous', 'manual'];
    let settled = {};
    const settings = MediaStreamTrack.prototype.getSettings;
    MediaStreamTrack.prototype.getCapabilities = function () {
      return { exposureMode: modes, whiteBalanceMode: modes, focusMode: modes };
    };
    // The three modes are stated rather than passed through, and it is load-bearing: Chromium's
    // fake device reports `exposureMode: 'manual'` from the moment it opens, so a fake that let
    // the real settings show through has `setLocks` find every lock already held and apply no
    // constraint at all — and the delay below, which is what makes this test's window, never runs.
    // The camera has to start out adapting for pinning it to be an event.
    MediaStreamTrack.prototype.getSettings = function () {
      const adapting = {
        exposureMode: 'continuous', whiteBalanceMode: 'continuous', focusMode: 'continuous',
      };
      return { ...settings.call(this), ...adapting, ...settled };
    };
    MediaStreamTrack.prototype.applyConstraints = function (constraints) {
      return new Promise((resolve) => setTimeout(() => {
        for (const asked of constraints?.advanced ?? []) settled = { ...settled, ...asked };
        resolve();
      }, 100));
    };
    // The arm's round trip, held open for a second so the camera can be taken *inside* it. Delaying
    // the request rather than the reply, because the request is the message this side can hold —
    // and holding it delays the whole trip, which is what the window is made of. A dispatch timed
    // against the lock writes instead would be a guess at when they finished.
    const post = Worker.prototype.postMessage;
    window.__armPosted = false;
    Worker.prototype.postMessage = function (message, transfer) {
      if (message && message.kind === 'call'
          && message.method === 'CaptureSessionManager.armBurst') {
        window.__armPosted = true;
        setTimeout(() => {
          if (transfer === undefined) post.call(this, message);
          else post.call(this, message, transfer);
        }, 1000);
        return undefined;
      }
      return transfer === undefined ? post.call(this, message) : post.call(this, message, transfer);
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await aimAtACell(page);
    await viewfinderIsLive(page);

    const armed = await page.evaluate(async () => {
      const capturing = window.sphanoramaCapture();
      // Waited for rather than timed: the arm is posted only once the lock writes have answered,
      // so this is the moment the third window opens.
      while (!window.__armPosted) await new Promise((r) => setTimeout(r, 10));
      const stream = document.querySelector('video').srcObject;
      for (const track of stream.getTracks()) track.dispatchEvent(new Event('ended'));
      return capturing;
    });
    expect(armed).toBe(false);

    // And the page's last word is the true one, two seconds after everything has settled.
    await expect(page.locator('#stage')).toContainText(/taken away/i, { timeout: 15000 });
    await page.waitForTimeout(2000);
    await expect(page.locator('#guidance')).not.toContainText(/capturing without/i);
    await expect(page.locator('#guidance')).not.toContainText(/not aimed at that cell/i);
    await expect(page.locator('#guidance')).toContainText(/taken away/i);
  } finally {
    await server.close();
    await context.close();
  }
});

test('a lock write that answers late leaves the row explaining the refusal', async ({ browser }) => {
  // `unlock()` snapshots the locks row to say what its release is a release *of*, and this branch
  // queued the release one statement before writing its own record — so the snapshot was the
  // previous burst's line, or, on the first arm of a session, the empty string. When the release
  // finally landed it painted that over the diagnosis: "no burst has run yet", on the path where a
  // burst was attempted and refused, deleting the only on-screen record of why.
  //
  // A camera that answers everything, slowly — 1500 ms per constraint — rather than one that never
  // answers. The 30 s camera in the test above is a dead track wearing a slow one's clothes: its
  // release needs minutes to land, so the row is still correct when the assertions run.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    const modes = ['continuous', 'manual'];
    const settings = MediaStreamTrack.prototype.getSettings;
    MediaStreamTrack.prototype.getCapabilities = function () {
      return { exposureMode: modes, whiteBalanceMode: modes, focusMode: modes };
    };
    // A camera that never *reaches* the mode it is asked for, so every key is asked in both
    // holding modes and the write is six constraints rather than one.
    //
    // Stated here rather than inherited from the device, which is the whole point: Chromium's fake
    // track already reports `manual` for exposure and focus, so a stub that echoes its settings
    // makes `holding()` true before anything is asked and `setLocks` issues at most two
    // constraints — inside the 3 s bound, and then the arm succeeds and this test is about nothing.
    // A reviewer read the missing echo as the bug; the echo is what breaks it, and the honest fix
    // is to pin the arrangement instead of depending on either default.
    //
    // 700 ms × 6 = 4.2 s for the arm, so the caller gives up at 3 s with the write still going;
    // the release queued behind it is three more and lands at 6.3 s, which is when the row is
    // judged. The first version used 1500 ms, where the release lands at 13.5 s — past where the
    // test stopped looking, which is why it passed against the defect it was written for.
    MediaStreamTrack.prototype.getSettings = function () {
      return {
        ...settings.call(this),
        exposureMode: 'continuous', whiteBalanceMode: 'continuous', focusMode: 'continuous',
      };
    };
    MediaStreamTrack.prototype.applyConstraints = function () {
      return new Promise((resolve) => setTimeout(resolve, 700));
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await aimAtACell(page);
    await viewfinderIsLive(page);

    expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(false);
    await expect(page.locator('#locks')).toContainText(/did not answer/i, { timeout: 15000 });

    // The release queued by the refusal lands a few seconds later. Whatever it appends, it must
    // append it to the diagnosis rather than replace it: measured before the fix as the row
    // flipping to "no burst has run yet" at t=7.5 s.
    await page.waitForTimeout(8000);   // past 6.3 s, where the release lands
    await expect(page.locator('#locks')).toContainText(/did not answer/i);
    await expect(page.locator('#locks')).not.toContainText('no burst has run yet');
  } finally {
    await server.close();
    await context.close();
  }
});

test('a camera that will not answer a lock request does not get a burst', async ({ browser }) => {
  // The chain behind `writeLocks` deliberately does not cancel a write it has stopped waiting for
  // — a cancelled one could be overtaken by the release queued behind it and end a session with
  // the camera locked. So a write the caller gave up on still reaches the track, and on a camera
  // that answers late it reaches it *during* the burst that would otherwise have been armed here:
  // five frames straddling an exposure and focus change, which is the failure ADR 0022 exists to
  // prevent, with the core told no locks were held and the row calling it a refusal.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    const modes = ['continuous', 'manual'];
    MediaStreamTrack.prototype.getCapabilities = function () {
      return { exposureMode: modes, whiteBalanceMode: modes, focusMode: modes };
    };
    // Late rather than dead: it resolves, well past the three seconds the page waits. A track that
    // never settles at all would prove less — the interesting case is the one where the write does
    // eventually land, because that is the one that can land inside a burst.
    MediaStreamTrack.prototype.applyConstraints = function () {
      return new Promise((resolve) => setTimeout(resolve, 30000));
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await aimAtACell(page);
    await viewfinderIsLive(page);

    // Refused, and no pixels banked. Before this, the timeout was manufactured into the same
    // `{ok: false, CameraUnavailable}` a camera that *says no* produces, and arming went ahead on
    // it — so this returned true and a cell was filled from a burst nobody could explain.
    expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(false);
    await expect(page.locator('#locks')).toContainText(/did not answer/i, { timeout: 15000 });
    // Counted late, and that is the whole of this assertion.
    //
    // A burst does not bank anything at the moment it is armed: five frames at 80 ms with a 150 ms
    // settle take about half a second, one per tick. Read straight after the arm, the count is 0
    // whether the burst was armed or refused — a reviewer removed the refusal, dropped this test's
    // two sibling assertions, and watched this one pass. With the wait it comes back 5.
    await page.waitForTimeout(4000);
    const captured = await countCandidates(page);
    expect(captured).toBe(0);
  } finally {
    await server.close();
    await context.close();
  }
});

test('calls a manager through the generated facade', async ({ page }) => {
  // The round trip end to end in a real browser: encode arguments, dispatch across the C ABI,
  // decode a Result. The unit tests cover each half against a fake; this is the only place both
  // halves and the real WASM build meet.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    // ProjectManager.list needs no resource-access port, so what it proves is the marshalling,
    // not a pipeline that is not built.
    await expect(page.locator('#facade')).toContainText(/\d+ methods · \d+ projects/,
                                                        { timeout: 15000 });
  } finally {
    await server.close();
  }
});

test('a manager failure crosses the boundary as a status, not a crash', async ({ page }) => {
  // A domain failure has to arrive as something the client can branch on. If it came back as a
  // trap the page would go blank with nothing to explain it.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    const outcome = await page.evaluate(async () => {
      const core = window.sphanoramaCore;
      const refused = await core.project.create('');          // an untitled project is refused
      const project = await core.project.create('a project to capture into');
      const spec = {
        strategy: 'Rings', horizontalFovDeg: 0, verticalFovDeg: 0, overlapTarget: 0.3,
        acceptanceConeDeg: 4, coverPoles: true, motion: 'None',
      };
      const started = await core.captureSession.begin(project.value, spec);
      const orphaned = await core.captureSession.begin(4040, spec);
      return {
        refusedCode: refused.ok ? null : refused.status.code,
        startedCode: started.ok ? null : started.status.code,
        startedDetail: started.ok ? null : started.status.detail,
        orphanedCode: orphaned.ok ? null : orphaned.status.code,
      };
    });
    expect(outcome.refusedCode).toBe('InvalidArgument');
    // Both ports are real now and nothing on this page has enabled either: no camera is open, and
    // the host has not been told a motion capability, so it answers `None`. Since ADR 0044 that
    // is the first refusal `Begin` reaches — it establishes the sensor before it asks for a
    // camera, so a session that cannot start never raises the permission prompt.
    expect(outcome.startedCode).toBe('SensorUnavailable');
    expect(outcome.startedDetail).toContain('motion');
    // And a session for a project nobody created never gets as far as either. That check is still
    // first: a `Begin` naming a project that does not exist would otherwise leave a titleless one
    // in the user's list.
    expect(outcome.orphanedCode).toBe('NotFound');
  } finally {
    await server.close();
  }
});

test('enabling plans a sphere sized from the camera and guides toward a cell', async ({ page }) => {
  // The whole capture chain in one go: the page opens a camera and tells the host, the core reads
  // it through a synchronous port, the planner tessellates for it, and the sensor loop comes back
  // with a target cell. Chromium's fake camera supplies the resolution; the field of view is the
  // documented assumption in capture-host.ts, not a measurement.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();

    await expect(page.locator('#stage')).toContainText(/\d+ cells planned/, { timeout: 15000 });
    // Guidance only appears once onMotion has answered, so this also proves the loop runs.
    await expect(page.locator('#guidance')).toContainText(/cell \d+/, { timeout: 15000 });

    const plan = await page.evaluate(async () => {
      const got = await window.sphanoramaCore.captureSession.getPlan();
      return got.ok ? got.value : null;
    });
    expect(plan).not.toBeNull();
    // A tessellation, not a placeholder: rings from pole to pole, denser around the equator.
    expect(plan.nodes.length).toBeGreaterThan(8);
    expect(plan.spec.horizontalFovDeg).toBeGreaterThan(0);
    expect(new Set(plan.nodes.map((n) => n.ringIndex)).size).toBeGreaterThan(2);
  } finally {
    await server.close();
  }
});

test('every cell of the plan can be aimed at, not just the one a test happens to pick',
  async ({ page }) => {
  // The helper the whole suite aims with, checked against the plan it will be asked for rather
  // than against the one cell a given test reaches. `aimAtACell` verifies its own round trip and
  // fails loudly, but only for the cell it picked — and it picks the first uncaptured one, which
  // on this tessellation is a pole. A reviewer found the horizon ring degenerate: elevation zero
  // is exactly the gimbal-lock singularity of the Z-X'-Y'' decomposition, so cells 13-19 threw
  // their azimuth away and would have failed the guard as an opaque fifteen-second timeout in
  // whichever test first reached one.
  //
  // Nothing about the app is under test here. It is the arithmetic the tests are written on, and
  // a harness that is wrong for a seventh of the sphere is one that decides what the suite is
  // able to check.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText(/\d+ cells planned/, { timeout: 15000 });

    const nodes = await page.evaluate(async () => {
      const plan = await window.sphanoramaCore.captureSession.getPlan();
      return plan.ok ? plan.value.nodes.map((n) => n.targetOrientation) : [];
    });
    expect(nodes.length).toBeGreaterThan(8);

    // And that the plan still puts a cell *at* the singularity, which is what makes this test about
    // anything. The rings engine always lays a ring on the horizon, and `beta = 90°` is exactly
    // that ring — but the test never said so, so a tessellation that stopped doing it (the
    // strategy enum already names `Geodesic`) would leave this sweeping cells that were never in
    // danger and reporting nothing.
    const betas = nodes.map((target) => deviceOrientationLookingAt(target).beta);
    expect(betas.some((beta) => Math.abs(beta - 90) < 0.001),
      `no cell of this plan sits at the gimbal-lock singularity; betas: ${betas.map((b) => b.toFixed(1)).join(', ')}`).toBe(true);

    const off = [];
    for (const [index, target] of nodes.entries()) {
      const { alpha, beta, gamma } = deviceOrientationLookingAt(target);
      const round = quaternionFromDeviceOrientation(alpha, beta, gamma, 0);
      const dot = Math.abs(round.w * target.w + round.x * target.x
        + round.y * target.y + round.z * target.z);
      if (dot < 0.9999) off.push(`${index} (|dot| ${dot.toFixed(4)}, beta ${beta.toFixed(1)})`);
    }
    expect(off, `cells the aim helper cannot reach: ${off.join(', ')}`).toEqual([]);
  } finally {
    await server.close();
  }
});

test('the coverage map shows the whole plan before anything is captured', async ({ page }) => {
  // A coverage map is most useful before a capture, not after: it is what tells you where to
  // point. Drawing it only when a cell completes — which is the only moment coverage *changes* —
  // means the panel is empty at exactly the moment it is worth reading, and the first thing it
  // ever shows is one cell filled in a field of nothing.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText(/\d+ cells planned/, { timeout: 15000 });

    // The panel folds itself away when a capture starts, so the picture is not covered while the
    // user is aiming. Reading what is inside it means opening it, which is what a person does.
    await page.locator('#panel-toggle').click();
    const cells = page.locator('#coverage-map .cell');
    await expect.poll(async () => cells.count(), { timeout: 15000 }).toBeGreaterThan(8);

    const planned = await page.evaluate(async () => {
      const got = await window.sphanoramaCore.captureSession.getPlan();
      return got.ok ? got.value.nodes.length : 0;
    });
    expect(await cells.count()).toBe(planned);
    // Every one of them a hole, because nothing has been captured yet — a map that opened with
    // cells already filled would be describing a capture that never happened.
    expect(await page.locator('#coverage-map .cell[data-state="hole"]').count()).toBe(planned);
  } finally {
    await server.close();
  }
});

test('the cell straight ahead can actually be clicked', async ({ page }) => {
  // The map draws a dashed guide down its centre, and the centre is where the straight-ahead cell
  // sits. Both it and the dots are absolutely positioned with no z-index, so the guide — a
  // generated box, painted after its siblings — is on top of the one dot a user is most likely to
  // reach for first. A decoration that eats the click is invisible in a unit test, because there
  // is no layout there to overlap: this needs a real browser hit-test, which is what a click is.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText(/\d+ cells planned/, { timeout: 15000 });

    await page.locator('#panel-toggle').click();
    const ahead = page.locator('#coverage-map .cell[style*="left: 50%"]').first();
    await expect(ahead).toBeVisible({ timeout: 15000 });
    // Playwright clicks the element's centre and refuses to click through something else, so an
    // intercepting guide fails here rather than being clicked instead of the dot.
    await ahead.click({ timeout: 5000 });

    // Nothing has been captured, so the strip says so — the assertion is that the click arrived
    // at the button at all.
    await expect(page.locator('#strip-heading')).toContainText(/nothing captured/i);

    // And the guide says outright that it is not interactive, rather than being harmless by
    // accident. It is currently a zero-width box with a one-pixel border, and that is the only
    // reason the dot wins the hit test today: give the guide a width to make it easier to see and
    // the click it swallows would be the one on the cell a capture starts from.
    const interactive = await page.evaluate(() => getComputedStyle(
      document.querySelector('#coverage-map'), '::after').pointerEvents);
    expect(interactive).toBe('none');
  } finally {
    await server.close();
  }
});

test('the cells you can see are marked in the viewfinder', async ({ page }) => {
  // The overlay end to end: the plan the core built, projected through the attitude the sensor
  // reported, into elements on the page. planOverlay's own tests cover which rings should exist
  // and where; what they cannot see is whether any of it reaches the DOM, which is the half that
  // has to survive a build.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText(/\d+ cells planned/, { timeout: 15000 });

    // The adapter hands over to the event because AbsoluteOrientationSensor refuses to start in
    // this browser; waiting for that is what makes the dispatch below land somewhere.
    await expect(page.locator('#motion-state')).toContainText('DeviceOrientation', {
      timeout: 15000,
    });
    // Level and facing forward, which is where the plan puts a whole ring of cells.
    await page.evaluate(() => {
      window.dispatchEvent(new DeviceOrientationEvent('deviceorientation', {
        alpha: 0, beta: 90, gamma: 0,
      }));
    });

    const rings = page.locator('#cell-layer .cell-ring:not([hidden])');
    await expect.poll(async () => rings.count(), { timeout: 15000 }).toBeGreaterThan(0);

    // Positioned as a fraction of the viewfinder, not left at the corner.
    const placed = await rings.first().evaluate((ring) => ({
      left: ring.style.left, top: ring.style.top,
    }));
    expect(placed.left).toMatch(/%$/);
    expect(placed.top).toMatch(/%$/);

    // And a fraction of the picture means a fraction of the *picture*. The layer used to stop
    // where the panel began, which squeezed the whole field of view into the strip above it: the
    // cell you were aiming at drew a third of a screen clear of the reticle sitting on it. What
    // the video occupies is what the markers are measured against, and nothing else is.
    const boxes = await page.evaluate(() => {
      const layer = document.querySelector('#cell-layer').getBoundingClientRect();
      const video = document.querySelector('#viewfinder').getBoundingClientRect();
      return { layer: [layer.left, layer.top, layer.width, layer.height],
               video: [video.left, video.top, video.width, video.height] };
    });
    expect(boxes.layer.map(Math.round)).toEqual(boxes.video.map(Math.round));
  } finally {
    await server.close();
  }
});

test('the off-screen arrow is not on screen when there is nothing to point at', async ({ page }) => {
  // The arrow is raised only when the target cell is *out of the picture*, and against a real
  // tessellation a level phone always has a cell in view — so on a fresh capture it should never
  // appear. It appeared always.
  //
  // `#target-arrow { display: grid }` is an author rule and the UA sheet's `[hidden] { display:
  // none }` is a lower origin, so `arrow.hidden = true` changed nothing that could be seen. And
  // because the painter also stops *updating* the arrow when there is no target to point at, what
  // stayed on screen was its last bearing and last distance, unchanging. That is the device report
  // — "the arrow only moves while I am pointing the phone down" — from the other side: it was
  // frozen the rest of the time, not absent.
  //
  // In the browser rather than in a unit test because the defect is a cascade one: the markup and
  // the class both say `hidden`, and only a real stylesheet in a real engine disagrees.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });

    // Before a capture even starts there is no target, so nothing to point at.
    await expect(page.locator('#target-arrow')).toBeHidden();

    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText(/\d+ cells planned/, { timeout: 15000 });
    await expect(page.locator('#motion-state')).toContainText('DeviceOrientation', {
      timeout: 15000,
    });

    const turn = async (alpha, beta = 90) => {
      await page.evaluate(([a, b]) => {
        window.dispatchEvent(new DeviceOrientationEvent('deviceorientation', {
          alpha: a, beta: b, gamma: 0,
        }));
      }, [alpha, beta]);
      await page.evaluate(() => new Promise((done) => requestAnimationFrame(() => done())));
    };

    for (let alpha = 0; alpha < 360; alpha += 15) {
      await turn(alpha);
      await expect(page.locator('#target-arrow'), `alpha ${alpha}`).toBeHidden();
    }

    // And it has to be able to appear, or `display: none` on the element unconditionally — the
    // feature deleted — would pass everything above. It did: a reviewer put that exact sabotage in
    // and the whole suite stayed green, because on a fresh capture the target is always on screen
    // and the arrow is correctly raised at none of 2664 attitudes.
    //
    // Capturing a cell is what creates the state: guidance then sends the user to a cell that is
    // still missing, and from where the capture was taken that cell is out of the picture.
    //
    // Level and facing forward, named rather than left wherever the sweep above finished, so the
    // assertion below is about a stated attitude instead of the loop's last value.
    const home = 0;
    await turn(home);
    // Through the hook rather than by pressing `#capture`, which ADR 0043 took away where there is
    // an aim: the dwell fires every burst now, and the button that remains is hidden and disabled
    // except on a device that cannot aim at all. Waiting for it to be enabled waited for ever.
    await expect(page.locator('#cell-layer .cell-ring:not([hidden])'))
      .toHaveCount(1, { timeout: 15000 });
    // The premise this test rests on, named rather than assumed: from here exactly one cell is in
    // view, so capturing it puts *every* remaining hole off screen and the arrow's condition is
    // met by construction. A runner reporting a different field of view would see two, and the
    // assertion below would then fail accusing the arrow of a defect that was really geometry.
    expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(true);
    await expect(page.locator('#guidance')).toContainText(/captured|cell done/i, { timeout: 15000 });

    // And the cell that was just shot is drawn as captured. The one ring in view is it, so this is
    // an assertion about a known cell rather than about whichever ring happened to be first.
    //
    // Read as a computed colour in a real browser, because the whole feature is a cascade: the
    // page's own rules and the UA's compete, and this PR's other defect was a UA rule losing to a
    // `display: grid` one selector away. `painter.test.ts` pins the attribute reaching the DOM
    // under happy-dom, which has no cascade to get wrong — so deleting both stylesheet rules left
    // the entire gate green and a captured cell pixel-identical to a hole.
    // Polled, because the `data-captured="true"` this selects on is written by `refreshCoverage`'s
    // answer — a worker round trip that starts on the `CellDone` tick and lands whenever it lands.
    // Read once, a loss reads as a CSS failure in the only cascade assertion the gate has.
    await expect(page.locator('#cell-layer .cell-ring[data-captured="true"]:not([hidden])'))
      .toHaveCount(1, { timeout: 15000 });
    const ringColours = await page.evaluate(() => {
      // `:not([hidden])` because a ring that has left the view keeps its element and its
      // attributes: without it this could be satisfied by a stale hidden ring rather than by the
      // one on screen, which is the only assertion in the suite that a captured cell *looks*
      // captured.
      const ring = document.querySelector('#cell-layer .cell-ring[data-captured="true"]:not([hidden])');
      if (ring === null) return null;
      return {
        fill: getComputedStyle(ring.querySelector('.ring-fill')).stroke,
        track: getComputedStyle(ring.querySelector('.ring-track')).stroke,
      };
    });
    // `--captured`, not `--accent`. Stated as the literal colours because that is what a person
    // looking at the screen is comparing, and because a test that read the custom property back
    // would pass against a rule that never applied.
    expect(ringColours).toEqual({ fill: 'rgb(142, 224, 106)', track: 'rgb(142, 224, 106)' });

    // **And the arrow's other half is gone from this test, deliberately.**
    //
    // It used to assert here that the arrow *can* appear, on a premise it stated out loud:
    // capturing the cell you are on sends guidance to a cell that is still missing, and from here
    // that cell is out of the picture. ADR 0041 deleted exactly that — guidance now names the cell
    // the camera is *inside*, captured or not — so the target never leaves the screen and
    // `planOverlay`'s condition (`isTarget && !seen.onScreen && !captured`) is not met.
    //
    // Measured over 247 attitudes covering the whole sphere, at three arrangements — fresh, after
    // capturing the cell in view, and after capturing a neighbourhood: the arrow is raised at none
    // of them. Whether it should point at `targetNode` at all, or at the nearest *hole*, is a
    // question about the feature rather than about this test, and it has an issue of its own.
    //
    // What stays is the half that pins the defect this test was written for: the cascade. The
    // markup and the class both said `hidden` and only a real stylesheet in a real engine
    // disagreed, and every assertion above still fails without `#target-arrow[hidden] { display:
    // none; }`.
    await turn(home);
    await expect(page.locator('#target-arrow')).toBeHidden();
  } finally {
    await server.close();
  }
});

test('holding a cell fires a burst with nobody pressing anything', async ({ page }) => {
  // ADR 0043, end to end: the core counts the dwell, reports it on the guidance the page already
  // reads, and the page arms on `Fire`. Nothing here presses anything.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText(/\d+ cells planned/, { timeout: 15000 });
    await expect(page.locator('#motion-state')).toContainText('DeviceOrientation', {
      timeout: 15000,
    });
    await viewfinderIsLive(page);

    // One attitude, held. The dwell only counts ticks a sample arrived on, so the samples have to
    // keep coming — which is what a phone in a hand does, and what a phone on a table does not.
    const hold = async () => {
      await page.evaluate(() => {
        window.dispatchEvent(new DeviceOrientationEvent('deviceorientation', {
          alpha: 0, beta: 90, gamma: 0,
        }));
      });
      await page.waitForTimeout(50);
    };
    await hold();
    await expect(page.locator('#guidance')).toContainText(/hold still/, { timeout: 15000 });
    // And no shutter anywhere on the page, because the dwell is the only way a burst starts now
    // (ADR 0044). Asserted as absence rather than as a hidden element: a `toBeHidden` on a
    // control that no longer exists passes for the wrong reason, and would go on passing if
    // somebody put a disabled one back.
    expect(await page.evaluate(() => document.querySelectorAll('button').length > 0)).toBe(true);
    expect(await page.evaluate(() => document.getElementById('capture'))).toBe(null);

    // Held, and watched while it is held. The ring has to be sampled *during* the hold rather than
    // after it: the dwell only advances on ticks a sample arrives on, so a poll that dispatches
    // nothing watches a dwell that is not running — which is how the first version of this test
    // failed, waiting fifteen seconds for a ring that had every reason to stay empty.
    const offsets = [];
    let fired = false;
    for (let i = 0; i < 120 && !fired; i += 1) {
      await hold();
      const seen = await page.evaluate(() => ({
        offset: (() => {
          const fill =
            document.querySelector('#cell-layer .cell-ring[data-target="true"] .ring-fill');
          return fill === null ? -1 : Number.parseFloat(fill.style.strokeDashoffset);
        })(),
        guidance: document.getElementById('guidance').textContent ?? '',
      }));
      offsets.push(seen.offset);
      fired = /captured|cell done|capturing/i.test(seen.guidance);
    }

    // A burst, with nobody pressing anything.
    expect(fired).toBe(true);
    await expect.poll(() => countCandidates(page), { timeout: 30000 }).toBe(5);

    // And the ring told the user it was coming. A part-drawn arc — neither empty nor full — is the
    // whole of what `heldFraction` buys: the number the core counted, on screen, before it fired.
    const partial = offsets.filter((offset) => offset > 0.5 && offset < 56);
    expect(partial.length, `offsets seen: ${offsets.join(', ')}`).toBeGreaterThan(0);
  } finally {
    await server.close();
  }
});

test('the page says which locks the burst actually got', async ({ page }) => {
  // The question a burst's numbers raise and the strip could not answer: is the camera free to
  // re-expose and refocus between these five frames? On a Pixel one cell's candidates scored
  // 1186, 1180, 459, 458, 458 — two frames from one regime and three from another — with nothing
  // on screen to say whether that was the camera hunting or the selection policy.
  //
  // Chromium's fake camera lists `["manual", "continuous"]` for exposure and for focus, and
  // nothing for white balance — measured, against the pinned browser this suite runs. So it holds
  // two of the three, and the row is the two it holds. White balance is not named at all: it was
  // never asked for, which is a different fact from a refusal and reads differently on purpose.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText(/\d+ cells planned/, { timeout: 15000 });
    await page.locator('#panel-toggle').click();

    // Nothing said yet: this is about a burst, and none has been fired.
    await expect(page.locator('#locks')).toHaveText('—');

    await aimAtACell(page);
    await viewfinderIsLive(page);
    expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(true);
    await expect(page.locator('#locks')).not.toHaveText('—', { timeout: 15000 });
    await expect(page.locator('#locks')).toHaveText('exposure · focus');
  } finally {
    await server.close();
  }
});

test('a refused lock says what the camera does offer', async ({ page }) => {
  // The Pixel row — `focus · exposure refused` — and the reason it was worth extending: it says a
  // lock did not take, and nothing about whether there was one to be had. This camera advertises
  // three exposure modes and then will not leave continuous, which is a camera contradicting
  // itself; the alternative reading, a camera with nothing to give, now looks different on screen
  // (ADR 0033).
  await page.addInitScript(() => {
    const settings = MediaStreamTrack.prototype.getSettings;
    let settled = {};
    MediaStreamTrack.prototype.getCapabilities = function () {
      return {
        exposureMode: ['continuous', 'manual', 'single-shot'],
        focusMode: ['continuous', 'manual'],
      };
    };
    // Whatever is asked, the exposure stays where it is — which is what `applyConstraints`
    // resolving and the mode not moving looks like from the page (ADR 0022).
    MediaStreamTrack.prototype.getSettings = function () {
      return { ...settings.call(this), ...settled, exposureMode: 'continuous' };
    };
    MediaStreamTrack.prototype.applyConstraints = async function (constraints) {
      for (const asked of constraints?.advanced ?? []) {
        if ('exposureMode' in asked) continue;
        settled = { ...settled, ...asked };
      }
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText(/\d+ cells planned/, { timeout: 15000 });
    await page.locator('#panel-toggle').click();

    await aimAtACell(page);
    await viewfinderIsLive(page);
    expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(true);
    await expect(page.locator('#locks')).not.toHaveText('—', { timeout: 15000 });
    await expect(page.locator('#locks'))
      .toHaveText('focus · exposure refused (offers continuous, manual, single-shot)');
  } finally {
    await server.close();
  }
});

test('a browser that will not say is not reported as a camera with nothing to give', async ({ page }) => {
  // A browser that answers nothing: `getCapabilities` is optional and a track may simply not have
  // it. Every lock is then unasked and unheld, which is exactly what a camera with no manual modes
  // looks like from here — and the row used to say so, out loud, about a camera nobody had
  // managed to ask (ADR 0033).
  //
  // The settings are stubbed as well as the capabilities, and that is the point of the stub
  // rather than a convenience: Chromium's fake device *starts* in manual and reports so, and a
  // browser that answers no question about its camera while the camera sits in a mode it was
  // never asked for is not a device that exists. What is being modelled is a camera adapting
  // freely and a browser with nothing to say about it, which is the pair this branch is for.
  await page.addInitScript(() => {
    delete MediaStreamTrack.prototype.getCapabilities;
    const settings = MediaStreamTrack.prototype.getSettings;
    MediaStreamTrack.prototype.getSettings = function () {
      return { ...settings.call(this), exposureMode: 'continuous', focusMode: 'continuous' };
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText(/\d+ cells planned/, { timeout: 15000 });
    await page.locator('#panel-toggle').click();

    await aimAtACell(page);
    await viewfinderIsLive(page);
    expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(true);
    await expect(page.locator('#locks')).not.toHaveText('—', { timeout: 15000 });
    await expect(page.locator('#locks')).toHaveText(/does not report/i);
    await expect(page.locator('#locks')).not.toHaveText(/no manual modes/i);
  } finally {
    await server.close();
  }
});

test('the ring fills when its cell finishes, without waiting for another sample', async ({ page }) => {
  // Coverage is refreshed *after* the tick that reports a cell done, so the tick that drew the
  // markers drew them from the coverage before it. Normally the next tick corrects that — but the
  // loop only asks for guidance when a sample arrives, and a phone held still through the end of
  // a burst gets no more. The map would fill in while the ring for that very cell stayed empty,
  // and nothing would ever put it right. This browser has no gyroscope, so it is that phone.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await aimAtACell(page);
    await viewfinderIsLive(page);

    // One sample and no more — which is what `aimAtACell` dispatches, and the whole arrangement
    // this test needs: nothing arrives after the burst, so the tick that reports the cell done is
    // the last tick there will ever be. A second dispatch here would be a second chance to redraw
    // and would quietly remove the thing under test.
    //
    // Waited for, not assumed. The sample has to be drained, folded into the pose and drawn
    // before the burst starts, or there are no rings for the burst to fill and the poll below
    // times out on a page that was only slow. Rings existing does not weaken what this is about:
    // no *further* sample is sent after the burst, which is the whole point.
    await expect(page.locator('#cell-layer .cell-ring:not([hidden])').first())
      .toBeAttached({ timeout: 15000 });

    expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(true);
    await expect(page.locator('#guidance')).toContainText(/captured|cell done/i, { timeout: 15000 });

    // A ring drawn as full: the dash offset closes to zero only at a fill of one.
    await expect.poll(async () => page.evaluate(() => {
      const fills = document.querySelectorAll('#cell-layer .cell-ring:not([hidden]) .ring-fill');
      return Array.from(fills).filter((arc) => Number(arc.style.strokeDashoffset) === 0).length;
    }), { timeout: 15000 }).toBeGreaterThan(0);
  } finally {
    await server.close();
  }
});

test('the markers follow the crop when the window changes shape', async ({ page }) => {
  // `object-fit: cover` scales the camera frame to fill its box and cuts off what hangs over, so
  // how much of the frame is on screen depends on the shape of the window. Whether a cell is in
  // shot is decided before any of that — so the same phone pointed the same way raises the same
  // rings either way, and only *where they are drawn* may move. If it does not move, the page is
  // handing the overlay no fit at all and every marker is short of the thing it names.
  const server = await serve();
  try {
    await page.setViewportSize({ width: 400, height: 900 });
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText(/\d+ cells planned/, { timeout: 15000 });
    await expect(page.locator('#motion-state')).toContainText('DeviceOrientation', {
      timeout: 15000,
    });

    // The video has to have reported a size before any of this means anything: an unmeasured
    // frame passes markers straight through, and two pass-throughs are equal for the wrong reason.
    await expect(page.locator('#camera-state')).toContainText(/\d+×\d+/, { timeout: 15000 });

    const aim = () => page.evaluate(() => {
      // Off the straight-ahead cell on purpose. The centre of the frame is the one point a crop
      // cannot move, so a test aimed dead at a cell would compare 50% with 50% and pass whatever
      // the page did.
      window.dispatchEvent(new DeviceOrientationEvent('deviceorientation', {
        alpha: 12, beta: 90, gamma: 0,
      }));
    });
    // Both axes. `cover` scales by whichever ratio is larger, so exactly one axis is ever cropped
    // — in a landscape window the horizontal ratio is 1 by construction — and a test watching only
    // `left` would be comparing two untouched numbers and calling the fit missing.
    const spots = () => page.evaluate(() => [...document.querySelectorAll(
      '#cell-layer .cell-ring:not([hidden])')].map((ring) => `${ring.style.left},${ring.style.top}`));

    await aim();
    await expect.poll(async () => (await spots()).length, { timeout: 15000 }).toBeGreaterThan(0);
    const tall = await spots();

    await page.setViewportSize({ width: 900, height: 400 });
    // Re-aimed inside the poll: the capture loop only redraws when a sample arrives, so a resize
    // on its own leaves the previous window's markers on screen for as long as the phone is still.
    await expect.poll(async () => {
      await aim();
      return (await spots()).join(' ');
    }, { timeout: 15000 }).not.toBe(tall.join(' '));
  } finally {
    await server.close();
  }
});

test('the panel gets out of the picture while a capture is running', async ({ page }) => {
  // It is more than half the screen, and the screen is what you aim with. Before this it sat over
  // the viewfinder for the whole of a capture, which is why the marker layer had been squeezed
  // into the strip above it — a workaround that moved every marker rather than the panel.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    const share = async () => page.evaluate(() => (
      document.querySelector('#panel').getBoundingClientRect().height
      / document.querySelector('#app').getBoundingClientRect().height));

    // Open to begin with: nothing is being aimed at yet, and this is where the camera is enabled.
    expect(await share()).toBeGreaterThan(0.25);
    // The label names the press and `aria-expanded` names the state, so they read as opposites and
    // have to stay in step with each other and with the panel.
    //
    // Read out of the served file, not off the page: the script sets both on startup, so by the
    // time the DOM can be queried it has already covered for whatever the markup said. What is
    // being checked here is the first paint — the page before any of this has run, which on a
    // phone on a slow connection is a real thing somebody sees.
    const markup = await (await page.request.get(server.appUrl)).text();
    const button = markup.match(/<button id="panel-toggle"[\s\S]*?<\/button>/)[0];
    expect(button).toContain('aria-expanded="true"');
    expect(button).toMatch(/>\s*hide\s*</);

    const toggle = page.locator('#panel-toggle');
    await expect(toggle).toHaveAttribute('aria-expanded', 'true');
    await expect(toggle).toHaveText('hide');

    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText(/\d+ cells planned/, { timeout: 15000 });
    await expect.poll(share, { timeout: 10000 }).toBeLessThan(0.25);
    await expect(toggle).toHaveAttribute('aria-expanded', 'false');
    await expect(toggle).toHaveText('details');

    // And it comes back on request, because everything it carries is still worth reading. Nothing
    // else reopens it: from the moment a capture starts, whether the picture is covered is the
    // user's call and not the app's.
    await toggle.click();
    await expect.poll(share, { timeout: 10000 }).toBeGreaterThan(0.25);
    await expect(toggle).toHaveAttribute('aria-expanded', 'true');
    await expect(toggle).toHaveText('hide');
  } finally {
    await server.close();
  }
});

test('the markers go when guidance stops working', async ({ page }) => {
  // Markers describe where cells are *relative to a pose*, and a failed tick produced no pose.
  // Leaving the last set on screen draws a confident answer over a line saying guidance has
  // stopped — so ending the session underneath the loop has to clear them, not freeze them.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText(/\d+ cells planned/, { timeout: 15000 });
    await expect(page.locator('#motion-state')).toContainText('DeviceOrientation', {
      timeout: 15000,
    });
    await page.evaluate(() => {
      window.dispatchEvent(new DeviceOrientationEvent('deviceorientation', {
        alpha: 0, beta: 90, gamma: 0,
      }));
    });

    const rings = page.locator('#cell-layer .cell-ring:not([hidden])');
    await expect.poll(async () => rings.count(), { timeout: 15000 }).toBeGreaterThan(0);

    // Pulled out from under the capture loop, which is what a failing tick looks like from here.
    // Then one more sample, because the loop only asks the core for guidance when there is
    // something new to fold in — without it the failure never happens and the line just stops.
    await page.evaluate(async () => { await window.sphanoramaCore.captureSession.end(); });
    await page.evaluate(() => {
      window.dispatchEvent(new DeviceOrientationEvent('deviceorientation', {
        alpha: 10, beta: 80, gamma: 0,
      }));
    });

    await expect(page.locator('#guidance')).toContainText('guidance failed', { timeout: 15000 });
    await expect.poll(async () => rings.count(), { timeout: 15000 }).toBe(0);
  } finally {
    await server.close();
  }
});

test('an orientation event moves the pose through the sensor port', async ({ page }) => {
  // The pull path end to end: a browser event lands in the adapter's buffer, the client hands it
  // to the host, IMotionSensorAccess::Drain reads it out of the heap as flat doubles, and
  // PoseEngine folds it in. A silently empty drain would leave guidance sitting on whatever cell
  // the identity orientation happens to be nearest, which is why the assertion is that the target
  // cell *changes* rather than that guidance merely exists.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#guidance')).toContainText(/cell \d+/, { timeout: 15000 });
    // AbsoluteOrientationSensor exists in this browser and refuses to start — there is no
    // gyroscope behind it — so the adapter hands over to the event. Waiting for that rather than
    // assuming it is what makes the dispatch below land somewhere, and it exercises the fallback
    // on a real engine rather than only against a fake.
    await expect(page.locator('#motion-state')).toContainText('DeviceOrientation', {
      timeout: 15000,
    });

    const before = await page.locator('#guidance').textContent();

    // Straight up. Whatever cell the phone starts on, it is not the zenith.
    await page.evaluate(() => {
      window.dispatchEvent(new DeviceOrientationEvent('deviceorientation', {
        alpha: 0, beta: 180, gamma: 0,
      }));
    });

    await expect(page.locator('#guidance')).not.toHaveText(before, { timeout: 15000 });
    // And the orientation readout followed the same samples, in the frame the plan is written in
    // rather than the triple the browser reported.
    await expect(page.locator('#orientation')).toContainText('el 90°');
  } finally {
    await server.close();
  }
});

test('the horizon rolls in place instead of swinging across the screen', async ({ page }) => {
  // A geometry contract between index.html and capture.css, and one that only a browser can
  // check: the client writes `rotate(deg 50 50)`, so the hub of the horizon group sits on the
  // centre of the reticle at every roll. Getting this wrong does not look like a rotation bug --
  // the marker sails across the viewfinder on an arc -- and nothing below the DOM can see it.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });

    const hubs = await page.evaluate(async () => {
      const group = document.getElementById('horizon-group');
      const hub = group.querySelector('circle');
      const settle = () => new Promise((done) => setTimeout(done, 200));
      const centres = [];
      // Past the transition each time: read straight after the write and the answer is where the
      // hub still is, not where it is going, and the test would pass on any transform at all.
      for (const deg of [0, -45, 45, 135, -170]) {
        group.setAttribute('transform', `rotate(${deg} 50 50)`);
        await settle();
        const box = hub.getBoundingClientRect();
        centres.push([box.x + box.width / 2, box.y + box.height / 2]);
      }
      return centres;
    });

    const [origin] = hubs;
    for (const [x, y] of hubs) {
      expect(Math.hypot(x - origin[0], y - origin[1])).toBeLessThan(1);
    }
  } finally {
    await server.close();
  }
});

test('a sensor that dies mid-session says so instead of going quiet', async ({ browser }) => {
  // Reported from a phone: motion simply stopped, with nothing on screen to say why. The
  // quaternion sensor reports a missing gyroscope or a refused grant asynchronously — long after
  // start() returned ok — and the fallback it hands over to can fail on its own, with no caller
  // left to return a failure to. The readout went to 'none' and that was the whole story.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    // Starts, then errors: Chromium's own way of reporting a device with no gyroscope, and the
    // one path where the failure arrives after the adapter has already claimed the source.
    window.AbsoluteOrientationSensor = class {
      constructor() { this.quaternion = null; this.timestamp = null; this.listeners = {}; }
      addEventListener(type, callback) { this.listeners[type] = callback; }
      start() { setTimeout(() => this.listeners.error?.(), 0); }
      stop() {}
    };
    // Nowhere to fall back to, which is what turns a handover into a dead session.
    delete window.DeviceOrientationEvent;
  });
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();

    // The reason the fallback could not take over, on the line that already carries the source.
    await expect(page.locator('#motion-state')).toContainText(/orientation events/i, {
      timeout: 15000,
    });
  } finally {
    await server.close();
    await context.close();
  }
});

test('a phone with no motion sensors is told what is required and what is missing',
  async ({ browser }) => {
  // Declining motion on iOS lands here, and so does any desktop without sensors. This test used
  // to be called `a phone with no motion sensors still captures` and asserted the opposite: the
  // core treated the device as a supported configuration, the pose went vision-only, and the page
  // started a session that said "capturing without motion — 32 cells planned, aim by hand".
  //
  // It did capture. What it produced was a folder of pictures with a plan's worth of guessed
  // labels: nothing anywhere verified that a cell's frames came from that cell's direction, and
  // the failure was invisible until a build stage this repo does not have yet. ADR 0044 refuses
  // it instead, and the message is the whole deliverable — it has to say that motion is required,
  // that this browser reports none, and the one thing worth trying, because for a user who
  // declined the prompt this is a choice they can unmake and nothing else on the page says so.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    // All three, because `detect()` reads all three: `DeviceMotionEvent` with either orientation
    // source is `GyroAccel`, either orientation source alone is `OrientationOnly`, and only the
    // absence of every one of them is `None`. Leaving one behind would test a phone that still
    // has a sensor.
    delete window.AbsoluteOrientationSensor;
    delete window.DeviceOrientationEvent;
    delete window.DeviceMotionEvent;
    // Counted, because the order these two questions are asked in is half of ADR 0044. The core
    // refuses before it opens a camera of its own — but the core's camera is this page's, already
    // opened, so the ordering that reaches a person is this one.
    window.__cameraAsked = 0;
    const media = navigator.mediaDevices;
    const real = media.getUserMedia.bind(media);
    media.getUserMedia = (constraints) => {
      window.__cameraAsked += 1;
      return real(constraints);
    };
  });
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();

    // Unavailable *and why*, which for this device is the honest answer that it has none. The
    // exact-text assertion this replaced was pinning the very thing that made an iPhone reading
    // unreadable: one word for a declined grant, an expired gesture and a phone with no sensors.
    await expect(page.locator('#motion-state')).toContainText('unavailable');
    await expect(page.locator('#motion-state')).toContainText(/no motion sensors/i);

    // And the sentence a person reads, on the line every other failure is reported on. Three
    // claims, asserted separately so a message that quietly loses one of them fails here.
    const stage = page.locator('#stage');
    await expect(stage).toContainText(/motion sensors/i, { timeout: 15000 });
    await expect(stage).toContainText(/needs/i);
    await expect(stage).toContainText(/settings/i);
    // Not the status code and not the component's own words.
    await expect(stage).not.toContainText('SensorUnavailable');
    // And no session behind it. Both halves: the word `beginSession` writes when one started, and
    // the plan the core would be holding if one had.
    await expect(stage).not.toContainText('capturing');
    // `-1` for "refused", not `0`: a plan with no cells and no plan at all are different answers,
    // and only one of them is what a refused session leaves behind. The sibling assertion in
    // `a camera that dies while the page is still enabling...` uses the same sentinel for the
    // same reason.
    const planned = await page.evaluate(async () => {
      const got = await window.sphanoramaCore.captureSession.getPlan();
      return got.ok ? got.value.nodes.length : -1;
    });
    expect(planned).toBe(-1);

    // And the camera was never asked for. A user who cannot capture must not be made to answer a
    // permission prompt on the way to being told so, which is the ordering ADR 0044 exists to get
    // right and the one the page — not the core — is the only place that can honour: `enable`
    // calls `getUserMedia` before the core is reached at all.
    expect(await page.evaluate(() => window.__cameraAsked)).toBe(0);

    // And nothing left to press. The offer goes rather than going grey: no press changes the
    // answer, and a live control under a sentence that says to change a setting and reload is the
    // same two-dead-buttons failure the resume path was fixed for one commit earlier.
    //
    // Only `#enable` is asserted here, and that is the point of the restraint. This page has no
    // stored capture, so `#resume` and `#new-capture` ship hidden and would pass a `toBeHidden`
    // whatever the code did — a reviewer showed that reverting `resumeButton.hidden` left those
    // two green. The resume offer is asserted where it can be false, in
    // `a stored capture is offered back and then withdrawn...` below.
    await expect(page.locator('#enable')).toBeHidden();
  } finally {
    await server.close();
    await context.close();
  }
});

test('a stored capture is offered back and then withdrawn on a device that cannot place frames',
  async ({ page }) => {
  // The resume half of ADR 0044, and the case that made the first version of the page's guard
  // wrong. `enable` is the one path a resume takes too, so the no-sensor branch returns before
  // `pickUp` runs — which means `describeResumeRefusal`'s own `SensorUnavailable` handling, added
  // in the same commit, is never reached here. The branch has to withdraw the offer itself, and
  // the first draft only re-enabled it: a live `#resume` under a sentence saying to change a
  // setting and reload, which is the two-dead-buttons failure that helper exists to prevent.
  //
  // One page throughout, because the session document has to survive into the sensorless load:
  // `addInitScript` applies from the next navigation, so the capture happens first and the
  // sensors are taken away across the reload.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await aimAtACell(page);
    await viewfinderIsLive(page);
    expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(true);
    await expect(page.locator('#guidance')).toContainText(/captured|cell done/i, { timeout: 15000 });
    await page.evaluate(() => window.sphanoramaHost.flush());

    await page.addInitScript(() => {
      delete window.AbsoluteOrientationSensor;
      delete window.DeviceOrientationEvent;
      delete window.DeviceMotionEvent;
      window.__cameraAsked = 0;
      const media = navigator.mediaDevices;
      const real = media.getUserMedia.bind(media);
      media.getUserMedia = (constraints) => {
        window.__cameraAsked += 1;
        return real(constraints);
      };
    });
    await page.reload();
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });

    // Offered, because whether a project has a session to come back to is a fact about the
    // project and is read without touching a sensor or a camera (ADR 0036).
    await expect(page.locator('#resume')).toBeVisible({ timeout: 15000 });
    await page.locator('#resume').click();

    // And withdrawn, with the reason, and without a camera prompt on the way.
    await expect(page.locator('#stage')).toContainText(/motion sensors/i, { timeout: 15000 });
    await expect(page.locator('#resume')).toBeHidden();
    await expect(page.locator('#enable')).toBeHidden();
    expect(await page.evaluate(() => window.__cameraAsked)).toBe(0);

    // The capture is still there. A refusal that took the document with it would turn "not on
    // this device" into "not ever".
    const stillThere = await page.evaluate(async () => {
      const listed = await window.sphanoramaCore.project.list();
      return listed.ok && listed.value.some((p) => p.hasSession);
    });
    expect(stillThere).toBe(true);
  } finally {
    await server.close();
  }
});

test('a project written through the core survives a reload', async ({ page }) => {
  // The whole point of the port, end to end: a manager in C++ writes through a synchronous
  // contract, the page persists it asynchronously behind that, and it is there next time.
  // Everything below the facade is exercised here — dispatch, codec, port, host, IndexedDB.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });

    const created = await page.evaluate(async () => {
      const result = await window.sphanoramaCore.project.create('kitchen sphere');
      // Durability is eventual by design (ADR 0014), so the test asks for it rather than
      // racing the flush timer.
      await window.sphanoramaHost.flush();
      return result.ok ? result.value : null;
    });
    expect(created).not.toBeNull();

    await page.reload();
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });

    const after = await page.evaluate(async () => {
      const listed = await window.sphanoramaCore.project.list();
      return listed.ok ? listed.value : null;
    });
    expect(after).not.toBeNull();
    expect(after.map((p) => p.title)).toContain('kitchen sphere');
    expect(after.map((p) => p.id)).toContain(created);
  } finally {
    await server.close();
  }
});

test('nothing to come back to means nothing is offered', async ({ page }) => {
  // The other half of the pair below, and the one that keeps the offer meaningful: a page that
  // proposed resuming on every load would train the button out of being read, and the state it
  // proposes resuming into does not exist.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await expect(page.locator('#resume')).toBeHidden();

    // And a project on its own is not a capture. `project.create` writes a title and nothing
    // else, so a page that read "a project exists" as "there is a session" would offer to resume
    // every sphere anyone ever named — including one whose capture never began.
    const created = await page.evaluate(async () => {
      const result = await window.sphanoramaCore.project.create('named but never captured');
      await window.sphanoramaHost.flush();
      return result.ok ? result.value : null;
    });
    expect(created).not.toBeNull();

    await page.reload();
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await expect(page.locator('#facade')).toContainText('1 projects');
    await expect(page.locator('#resume')).toBeHidden();
    await expect(page.locator('#stage')).toContainText('enable the camera to continue');
  } finally {
    await server.close();
  }
});

test('a capture interrupted by a reload is offered back with its cells', async ({ page }) => {
  // The whole point of the machinery, from the outside: capture a cell, lose the tab, and find
  // the sphere still there. Everything under it is covered against fakes — the document, the
  // replan, the adoption (ADR 0029), the durable spill index (ADR 0030) — and this is the only
  // place they meet a real browser, a real OPFS file and a page the user has to press.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });

    await aimAtACell(page);
    await viewfinderIsLive(page);
    expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(true);
    await expect(page.locator('#guidance')).toContainText(/captured|cell done/i, { timeout: 15000 });
    const before = await page.evaluate(async () => {
      const state = await window.sphanoramaCore.captureSession.coverage();
      // Asked for rather than raced: durability is eventual by design (ADR 0014), and the
      // reload below is exactly the event that does not wait for a flush timer.
      await window.sphanoramaHost.flush();
      return state.ok ? state.value.nodesSatisfied : 0;
    });
    expect(before).toBeGreaterThan(0);

    await page.reload();
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    // Offered without anything having been tried: no camera has been opened on this load, which
    // is the property the flag exists for (ADR 0036).
    await expect(page.locator('#resume')).toBeVisible();
    expect(await page.evaluate(() => document.querySelector('video').srcObject)).toBeNull();

    await page.locator('#resume').click();
    await expect(page.locator('#stage')).toContainText('resumed', { timeout: 15000 });

    const after = await page.evaluate(async () => {
      const state = await window.sphanoramaCore.captureSession.coverage();
      const plan = await window.sphanoramaCore.captureSession.getPlan();
      let restored = 0;
      for (const node of plan.value.nodes) {
        const got = await window.sphanoramaCore.captureSession.candidates(node.id);
        if (got.ok) restored += got.value.length;
      }
      return { satisfied: state.ok ? state.value.nodesSatisfied : 0, restored };
    });
    expect(after.satisfied).toBe(before);
    // The candidates came back too, not just the count of cells. A resume that restored the
    // coverage map and nothing else is the artefact ADR 0029 refused to produce: a sphere that
    // says it is captured and builds into nothing.
    expect(after.restored).toBe(5);
  } finally {
    await server.close();
  }
});

test('a resume the core refuses says why and still lets a new capture start', async ({ page }) => {
  // `Resume` can honestly say no — a document from a shape this build does not read is the case
  // ADR 0029 named, and it keeps the document rather than deleting it. What the page must not do
  // is offer a resume, have it refused, and leave nothing to press: the reason goes on screen and
  // starting over stays one press away.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });

    const created = await page.evaluate(async () => {
      const result = await window.sphanoramaCore.project.create('interrupted');
      await window.sphanoramaHost.flush();
      return result.ok ? result.value : null;
    });
    expect(created).not.toBeNull();

    // Written underneath the core rather than through it, because no contract writes a document
    // this shape — that is the point of it. The store is hydrated once at startup, so this has to
    // land before the reload that reads it.
    await page.evaluate(async (project) => {
      const db = await new Promise((resolve, reject) => {
        const request = indexedDB.open('sphanorama', 1);
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
      });
      await new Promise((resolve, reject) => {
        const transaction = db.transaction('documents', 'readwrite');
        transaction.objectStore('documents').put('sphanorama-session 99\n', `${project}/session`);
        transaction.oncomplete = resolve;
        transaction.onerror = () => reject(transaction.error);
      });
    }, created);

    await page.reload();
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await expect(page.locator('#resume')).toBeVisible();

    await page.locator('#resume').click();
    // The component's own words, not a sentence about https: `Unsupported` means one thing in the
    // camera adapter and another everywhere else, and only the first is about a secure origin.
    await expect(page.locator('#stage')).toContainText('Could not resume', { timeout: 15000 });
    await expect(page.locator('#stage')).toContainText('this build cannot read');
    await expect(page.locator('#stage')).not.toContainText('https');

    // And the offer is gone. Nothing this user can press changes which build is running, so an
    // offer left up is one that fails identically every time (ADR 0039). `enable` had already
    // hidden it on the way in, so what this pins is that the refusal did not put it back — which
    // is exactly what the other refusal below does.
    await expect(page.locator('#resume')).toHaveJSProperty('hidden', true);

    // And not stranded: a new sphere from here, on the camera the refused resume already opened.
    await expect(page.locator('#new-capture')).toBeVisible();
    await page.locator('#new-capture').click();
    await expect(page.locator('#stage')).toContainText(/capturing.*cells planned/,
                                                       { timeout: 15000 });
    await aimAtACell(page);

    // And gone once that capture is running. It is the only thing on screen that starts a render
    // loop, so leaving it pressable would let a second one run over the same session — two sets
    // of frame callbacks draining the same sensor and drawing the same overlay.
    //
    // The property rather than `toBeHidden`, which was the first thing written here and could not
    // fail: `beginSession` folds the panel this button sits in, so it is invisible either way and
    // the assertion passed with nothing hiding it at all.
    await expect(page.locator('#new-capture')).toHaveJSProperty('hidden', true);
  } finally {
    await server.close();
  }
});

test('a guidance call that rejects does not take the capture loop with it', async ({ browser }) => {
  // `onMotion` does not answer a worker-side failure with `{ok: false}` — it answers with a
  // rejected promise, and `step` awaited it bare. The trailing `requestAnimationFrame(step)` then
  // never runs and the loop ends without a word: the `else` branch that clears the markers is on
  // the resolved path, so what stays on screen is the full field of rings this PR taught the user
  // to read, frozen, under a guidance line still reporting the last answer that worked.
  //
  // Not a hypothetical trigger: `facade.ts` throws when `_malloc` returns 0, which its own comment
  // calls a real outcome on a phone that already has a sphere of frames pinned.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    // Injected where the worker would produce it — a `failed` reply to one `call` — rather than by
    // breaking the core, so what is exercised is the page's handling of a rejection and nothing
    // else. The third guidance call, so the loop is running and the failure is not the first
    // answer the page ever gets.
    const post = Worker.prototype.postMessage;
    // Armed by the test rather than counted here, because the loop asks for guidance once per
    // batch of samples and a batch is however many events landed in one animation frame. A
    // "fail the third call" rule made the injection depend on that timing, and on a fast run
    // there was no third call to fail.
    window.__failNextGuidance = false;
    window.__guidanceFailuresInjected = 0;
    Worker.prototype.postMessage = function (message, transfer) {
      if (message && message.kind === 'call'
          && message.method === 'CaptureSessionManager.onMotion'
          && window.__failNextGuidance) {
        window.__failNextGuidance = false;
        window.__guidanceFailuresInjected += 1;
        setTimeout(() => this.dispatchEvent(new MessageEvent('message', {
          data: {
            kind: 'failed', seq: message.seq,
            detail: "core could not allocate 64 bytes for 'CaptureSessionManager.onMotion'",
          },
        })), 0);
        return undefined;
      }
      // `undefined` is not an empty transfer list to `postMessage`, it is a type error — so the
      // two-argument call has to be reconstructed rather than forwarded blindly.
      return transfer === undefined ? post.call(this, message) : post.call(this, message, transfer);
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await expect(page.locator('#motion-state')).toContainText('DeviceOrientation', {
      timeout: 15000,
    });

    // The loop asks for guidance when a sample arrives, and this runner has no sensor of its own —
    // so the ticking is driven from here. Without it there is exactly one guidance call in a whole
    // session and nothing to observe: measured at 1 call in 2 s on this runner, which is how this
    // test's first draft managed to inject nothing at all.
    const pan = async (alpha) => {
      await page.evaluate((a) => {
        window.dispatchEvent(new DeviceOrientationEvent('deviceorientation', {
          alpha: a, beta: 90, gamma: 0,
        }));
      }, alpha);
    };
    // A normal answer first, so the failure below is not the first thing the page ever hears.
    await pan(10);
    await expect(page.locator('#guidance')).toContainText(/cell \d+/, { timeout: 15000 });

    // Every line `#guidance` shows from here on, because the recovery this test is *for* makes the
    // failure line short-lived: the loop keeps ticking through the samples already queued, so the
    // next successful answer overwrites it within a frame or two. Polling for the text raced that
    // and passed one run in three — which would have read as flakiness rather than as the loop
    // working exactly as intended.
    await page.evaluate(() => {
      window.__guidanceLines = [];
      const row = document.getElementById('guidance');
      new MutationObserver(() => window.__guidanceLines.push(row.textContent))
        .observe(row, { childList: true, characterData: true, subtree: true });
    });
    await page.evaluate(() => { window.__failNextGuidance = true; });
    for (let alpha = 20; alpha < 80; alpha += 10) {
      await pan(alpha);
      if (await page.evaluate(() => window.__guidanceFailuresInjected) > 0) break;
    }
    await expect.poll(() => page.evaluate(() => window.__guidanceFailuresInjected),
                      { timeout: 15000 }).toBe(1);
    // Said out loud rather than swallowed, and with the worker's own reason.
    await expect.poll(() => page.evaluate(
      () => window.__guidanceLines.some((line) => /guidance failed/i.test(line))),
      { timeout: 15000 }).toBe(true);
    expect(await page.evaluate(
      () => window.__guidanceLines.some((line) => /could not allocate/i.test(line)))).toBe(true);

    // The loop is still turning. This is the assertion the whole finding is about: a loop that had
    // died would leave the failure line up for ever, which on screen reads exactly like a loop
    // that recovered and then had nothing more to say — so what is asserted is a *later* line, not
    // the absence of the failure one.
    for (let alpha = 90; alpha < 170; alpha += 10) await pan(alpha);
    await expect.poll(() => page.evaluate(() => {
      const lines = window.__guidanceLines;
      const failed = lines.findIndex((line) => /guidance failed/i.test(line));
      return failed >= 0 && lines.slice(failed + 1).some((line) => /cell \d+/.test(line));
    }), { timeout: 15000 }).toBe(true);
    await expect(page.locator('#cell-layer .cell-ring:not([hidden])'))
      .not.toHaveCount(0, { timeout: 15000 });
  } finally {
    await server.close();
    await context.close();
  }
});

test('a guidance call that rejects mid-burst does not abandon the burst', async ({ browser }) => {
  // The other half of routing a rejection into the refusal branch, and the half that was wrong.
  // Clearing `firing` and `armed` there rests on the manager disarming an armed burst on every
  // failing tick — true of every answer that *reached* it, and false of a rejection, which means
  // the call threw on the way in and the manager never ran. The burst is then still armed inside
  // the core, its frames still pinned, its locks still applied.
  //
  // What clearing them costs is that the burst *stalls*, and the size of the stall is the whole
  // finding. A burst advances one frame per tick and on nothing else (ADR 0018), and the page
  // grabs the frame it advances over only `if (armed || firing)` — so with the flags dropped, the
  // tick gate falls back to the 250 ms heartbeat and the burst stops dead for a quarter of a
  // second while the camera's exposure lock is held.
  //
  // This test used to argue something stronger and false: that the two flags were "the only true
  // terms left in the tick gate" on a phone producing no motion samples, so clearing them left the
  // capture "dead for good — stuck part way through a burst, locks held". That has not been true
  // since the 250 ms heartbeat arrived. `quiet` is a fifth term, it reopens the gate by itself,
  // and the tick it opens answers `Firing`, which sets `firing` straight back. A reviewer found
  // the premise and the measurement settled it: with the hold neutered this test was green, five
  // candidates banked, in 2.6 s. Both the candidate count and the number of preview frames the
  // page pushed were still in the same range (21 against 30) — neither could tell the two apart.
  //
  // So the assertion is the one thing the hold actually decides: whether the loop asks for
  // guidance again on the *next animation frame* or a quarter of a second later. Counted over the
  // 120 ms after the rejection, that is about seven calls against none, which is a gap no runner's
  // load can close from either side. No orientation events are dispatched here, which keeps this
  // on the sample-less configuration UC-4 describes — the one where the heartbeat is the only
  // other term in the gate.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    const post = Worker.prototype.postMessage;
    window.__failNextGuidance = false;
    window.__guidanceFailuresInjected = 0;
    // Every guidance call the loop makes, and how many of them fall in the 120 ms after the
    // rejection — the window in which a held burst keeps ticking and a dropped one is waiting on
    // the heartbeat.
    window.__guidanceCalls = 0;
    window.__callsRightAfterTheRejection = null;
    Worker.prototype.postMessage = function (message, transfer) {
      if (message && message.kind === 'call'
          && message.method === 'CaptureSessionManager.onMotion') {
        window.__guidanceCalls += 1;
      }
      if (message && message.kind === 'call'
          && message.method === 'CaptureSessionManager.onMotion'
          && window.__failNextGuidance) {
        window.__failNextGuidance = false;
        window.__guidanceFailuresInjected += 1;
        // Snapshotted here rather than read from the test, because the window opens at the moment
        // the failing call is made: `lastGuidedMs` was set by this very call, so the heartbeat's
        // 250 ms is counted from now.
        const before = window.__guidanceCalls;
        setTimeout(() => { window.__callsRightAfterTheRejection = window.__guidanceCalls - before; },
                   120);
        setTimeout(() => this.dispatchEvent(new MessageEvent('message', {
          data: {
            kind: 'failed', seq: message.seq,
            detail: "core could not allocate 64 bytes for 'CaptureSessionManager.onMotion'",
          },
        })), 0);
        return undefined;
      }
      return transfer === undefined ? post.call(this, message) : post.call(this, message, transfer);
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    // Waiting for guidance to name a cell rather than for `#capture` to light up: ADR 0044 deleted
    // that button, and this test came from a branch where it was the readiness signal.
    await expect(page.locator('#guidance')).toContainText(/cell \d+/, { timeout: 15000 });
    // Aimed, because ADR 0041 refuses a burst at a cell the camera is not pointing at and this
    // test needs a real one in flight. It predates that rule and used to arm from wherever the
    // fake camera happened to be looking.
    await aimAtACell(page);
    await viewfinderIsLive(page);

    // Armed, then the very next guidance call is made to reject — so the failure lands with a
    // burst genuinely in flight, which is the state the argument is about.
    await page.evaluate(() => {
      window.__capturing = window.sphanoramaCapture();
      window.__failNextGuidance = true;
    });
    await expect.poll(() => page.evaluate(() => window.__guidanceFailuresInjected),
                      { timeout: 15000 }).toBe(1);
    // The burst finishes anyway.
    await expect(page.locator('#guidance')).toContainText(/captured|cell done/i, { timeout: 15000 });
    expect(await page.evaluate(() => window.__capturing)).toBe(true);
    const banked = await page.evaluate(async () => {
      const plan = await window.sphanoramaCore.captureSession.getPlan();
      let total = 0;
      for (const node of plan.value.nodes) {
        const got = await window.sphanoramaCore.captureSession.candidates(node.id);
        if (got.ok) total += got.value.length;
      }
      return total;
    });
    expect(banked).toBe(5);

    // And it did not stall getting there. This is the assertion the flags decide: held, the loop
    // asks again on the next animation frame; dropped, it waits for the heartbeat and the burst
    // sits for 250 ms holding the camera's exposure lock. Measured at about seven calls against
    // none, so `>= 2` is a threshold neither a fast runner nor a loaded one can cross by accident.
    const kept = await page.evaluate(() => window.__callsRightAfterTheRejection);
    expect(kept, 'the rejection stalled the burst until the heartbeat reopened the tick gate')
      .toBeGreaterThanOrEqual(2);
  } finally {
    await server.close();
    await context.close();
  }
});

test('a core that stops answering stops the loop rather than feeding it for ever', async ({ browser }) => {
  // The other end of holding `firing`/`armed` across a rejection. Held for one, a burst survives an
  // allocation that succeeds next time — which is the case the hold exists for. Held for every one,
  // a worker that is gone (`remote-core`'s `dead` is never cleared, and an Emscripten `abort()`
  // makes every later call throw) leaves the loop grabbing and transferring the preview frame at
  // about 4.9 MB a frame for the life of the page, on the very phone whose allocation failure
  // caused it. Before the flags were held at all, the first rejection stopped that — so an
  // unbounded hold is a worse outcome than the bug it fixes, for the same device.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    const post = Worker.prototype.postMessage;
    window.__failAllGuidance = false;
    window.__guidanceFailuresInjected = 0;
    Worker.prototype.postMessage = function (message, transfer) {
      if (message && message.kind === 'call'
          && message.method === 'CaptureSessionManager.onMotion'
          && window.__failAllGuidance) {
        window.__guidanceFailuresInjected += 1;
        setTimeout(() => this.dispatchEvent(new MessageEvent('message', {
          data: { kind: 'failed', seq: message.seq, detail: 'the core is gone' },
        })), 0);
        return undefined;
      }
      return transfer === undefined ? post.call(this, message) : post.call(this, message, transfer);
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await expect(page.locator('#guidance')).toContainText(/cell \d+/, { timeout: 15000 });
    await viewfinderIsLive(page);

    // Aimed, because a burst in flight is the premise: `firing`/`armed` are what keep the loop
    // ticking, which is exactly the state in which an unbounded hold never lets go. Without an aim
    // ADR 0041 refuses the arm and there is no burst — which is how this test ran for a round,
    // asserting a stopped loop refuses an arm that was already being refused for a different
    // reason entirely.
    await aimAtACell(page);
    const armed = await page.evaluate(async () => {
      window.__capturing = window.sphanoramaCapture();
      return window.__capturing;
    });
    expect(armed, 'no burst was ever in flight, so this test is about nothing').toBe(true);
    await page.evaluate(() => { window.__failAllGuidance = true; });

    await expect(page.locator('#stage')).toContainText(/stopped answering/i, { timeout: 15000 });
    await expect(page.locator('#cell-layer .cell-ring:not([hidden])')).toHaveCount(0);
    // The shutter this used to assert on is gone (ADR 0044). What replaces it is the thing the
    // button's disabling stood for: nothing can arm any more. `captureCell` is what the dwell and
    // the end-to-end hook both go through, and a stopped loop must refuse it.
    //
    // The refusal has to be *this* refusal, not any refusal. A reviewer showed the assertion
    // passing on a run where nothing had been armed at all, because `ArmBurst` was declining the
    // aim — and meanwhile that same call wrote four more `applyConstraints` to a live track after
    // the page had said the core was gone. So the line the guard produces is asserted too.
    expect(await page.evaluate(() => window.sphanoramaCapture()),
      'a stopped loop still armed a burst').toBe(false);
    await expect(page.locator('#guidance'))
      .toContainText(/stopped answering/i, { timeout: 5000 });

    // And it really stopped: no further guidance calls after the ones it took to decide.
    const settled = await page.evaluate(() => window.__guidanceFailuresInjected);
    await page.waitForTimeout(1500);
    expect(await page.evaluate(() => window.__guidanceFailuresInjected)).toBe(settled);
  } finally {
    await server.close();
    await context.close();
  }
});

test('a resume refused by the tier stays on offer, and goes when a capture starts', async ({ page }) => {
  // The other half of ADR 0039. A tier mismatch says this tier is not the one those frames went
  // into, which is not the same as saying they are gone: a session that fell back to a tier of its
  // own (ADR 0030) says exactly this while its pixels are still on disk, and the next run that
  // gets the resident pair resumes them. So the offer survives its own refusal here, where it did
  // not for a document shape this build cannot read.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await aimAtACell(page);
    await viewfinderIsLive(page);
    expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(true);
    await expect(page.locator('#guidance')).toContainText(/captured|cell done/i, { timeout: 15000 });

    // A real session document, written by the manager at the cell it just committed, and then one
    // line of it changed. The shape stays valid — that is the point: this has to refuse at the
    // tier comparison rather than at the parse, which is the refusal the other test covers.
    const project = await page.evaluate(async () => {
      await window.sphanoramaHost.flush();
      const listed = await window.sphanoramaCore.project.list();
      const withSession = listed.ok ? listed.value.filter((p) => p.hasSession) : [];
      return withSession.length === 1 ? withSession[0].id : null;
    });
    expect(project).not.toBeNull();

    const rewritten = await page.evaluate(async (id) => {
      const db = await new Promise((resolve, reject) => {
        const request = indexedDB.open('sphanorama', 1);
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
      });
      const key = `${id}/session`;
      const document = await new Promise((resolve, reject) => {
        const request = db.transaction('documents').objectStore('documents').get(key);
        request.onsuccess = () => resolve(request.result);
        request.onerror = () => reject(request.error);
      });
      if (typeof document !== 'string' || !/^tier \d+$/m.test(document)) return null;
      const changed = document.replace(/^tier \d+$/m, 'tier 999999999');
      await new Promise((resolve, reject) => {
        const transaction = db.transaction('documents', 'readwrite');
        transaction.objectStore('documents').put(changed, key);
        transaction.oncomplete = resolve;
        transaction.onerror = () => reject(transaction.error);
      });
      return changed;
    }, project);
    // Asserted rather than assumed: a document whose tier line this did not find would resume
    // perfectly well, and every expectation below would then be about the wrong thing.
    expect(rewritten).toContain('tier 999999999');

    await page.reload();
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await expect(page.locator('#resume')).toBeVisible();

    await page.locator('#resume').click();
    await expect(page.locator('#stage')).toContainText('spill tier', { timeout: 15000 });
    await expect(page.locator('#stage')).toContainText('try again');
    // Still there, and pressable. `enable` hid it on the way in and the refusal put it back.
    await expect(page.locator('#resume')).toBeVisible();
    await expect(page.locator('#resume')).toBeEnabled();

    // And pressing it again really does try again. The camera the first press opened is still
    // held, which is the fact that sends the second press straight at the session rather than
    // back through `enable` — asserted rather than assumed, because the branch is chosen by it.
    expect(await page.evaluate(() => document.querySelector('video').srcObject !== null)).toBe(true);
    // Counted from here, because "retries the session rather than the enabling" is a claim about
    // what the second press does *not* do. Both branches end at the same refusal on screen, so
    // the only thing that tells them apart is whether the camera was asked for a second time.
    await page.evaluate(() => {
      window.__cameraOpens = 0;
      const real = navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);
      navigator.mediaDevices.getUserMedia = (...args) => {
        window.__cameraOpens += 1;
        return real(...args);
      };
    });
    // The stage is cleared first because the second refusal reads exactly like the first, so an
    // assertion on that sentence would pass against a button whose handler did nothing at all.
    await page.evaluate(() => { document.getElementById('stage').textContent = 'cleared'; });
    await page.locator('#resume').click();
    await expect(page.locator('#stage')).toContainText('spill tier', { timeout: 15000 });
    expect(await page.evaluate(() => window.__cameraOpens)).toBe(0);

    // Until something starts. A live offer beside a running capture is a second render loop over
    // the same session one press away, which is the invariant `pump` holds for both buttons.
    await expect(page.locator('#new-capture')).toBeVisible();
    await page.locator('#new-capture').click();
    await expect(page.locator('#stage')).toContainText(/capturing.*cells planned/,
                                                       { timeout: 15000 });
    await expect(page.locator('#resume')).toHaveJSProperty('hidden', true);
  } finally {
    await server.close();
  }
});

test('deleting a project removes it for good', async ({ page }) => {
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });

    const remaining = await page.evaluate(async () => {
      const core = window.sphanoramaCore;
      const created = await core.project.create('to be deleted');
      await core.project.delete(created.value);
      await window.sphanoramaHost.flush();
      const listed = await core.project.list();
      return listed.value.map((p) => p.title);
    });
    expect(remaining).not.toContain('to be deleted');

    await page.reload();
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    const afterReload = await page.evaluate(async () => {
      const listed = await window.sphanoramaCore.project.list();
      return listed.value.map((p) => p.title);
    });
    expect(afterReload).not.toContain('to be deleted');
  } finally {
    await server.close();
  }
});

test('the shell really works offline, not just registers a worker', async ({ browser }) => {
  // The previous version of this test asserted only that a registration existed, which it did —
  // against a worker whose install handler opened an empty cache. It passed for a build that
  // could not serve a single byte offline. What has to be proved is a navigation with the
  // network cut.
  const context = await browser.newContext();
  const page = await context.newPage();
  const server = await serve();
  let closed = false;
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });

    // Registration is deliberately not awaited by the app — offline support must never delay
    // first paint — so wait for the worker to take control.
    //
    // The predicate is synchronous on purpose. An async one returns a Promise, and a Promise is
    // truthy, so waitForFunction succeeds on the first poll without waiting for anything: the
    // test this replaces used that pattern, which is half of why it passed against a worker that
    // cached nothing.
    await page.waitForFunction(() => navigator.serviceWorker.controller !== null, null,
                               { timeout: 15000 });

    // The server is shut down rather than the context put in offline mode: what a user loses is
    // the origin, and a dead origin is the case the cache has to cover. It also keeps the
    // service worker in the navigation path, which network emulation does not reliably do.
    await server.close();
    closed = true;
    await page.reload();

    // The whole app, from a cold navigation with nothing to reach: the shell, the bundle, and
    // the WASM core — the entry most likely to be missing, since it is staged in after the
    // build and would not appear on a hand-written precache list.
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await expect(page.locator('#core-caps')).toContainText('SIMD');
  } finally {
    if (!closed) await server.close();
    await context.close();
  }
});

test('a redeploy does not evict a cache that is still current', async ({ page }) => {
  // The cache name is a hash of the built bytes, so an unchanged rebuild keeps its warm cache and
  // any real change gets a new one. A fixed name — what this shipped with — meant the first build
  // a user saw was served to them forever.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    const worker = await page.request.get(new URL('sw.js', server.appUrl).href);
    const source = await worker.text();
    expect(source).toMatch(/const CACHE = 'sphanorama-shell-[0-9a-f]{16}'/);
    expect(source).not.toContain('__BUILD_ID__');
    expect(source).not.toContain('__PRECACHE__');
    // And the core is on the precache list, not just the HTML.
    expect(source).toContain('sphanorama-core.wasm');
  } finally {
    await server.close();
  }
});
