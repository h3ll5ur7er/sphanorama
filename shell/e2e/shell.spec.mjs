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
 * `#capture` becomes enabled when guidance says `HoldStill` — a fact about where the camera is
 * pointing, not about whether it is producing frames. The grabber refuses a video with no data or no dimensions and the loop only grabs at all
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
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

    // The same tier, so the resume runs the whole way and stops at the camera — which nothing on
    // this fresh page has opened yet. Every earlier step had to pass to get here: the document was
    // written (so the tier answered when it was checkpointed), it parsed, its token matched the
    // one the index came back with, and the store took every frame it names. A resume refused at
    // the tier would say FailedPrecondition instead, and one whose checkpoint never wrote a
    // document would say NotFound.
    const sameTier = await page.evaluate(
      (id) => window.sphanoramaCore.captureSession.resume(id), first);
    expect(sameTier.ok).toBe(false);
    expect(sameTier.status.code).toBe('CameraUnavailable');

    // And now a different sphere is started on this device, which empties the tier (ADR 0034) and
    // fills it again from identity 1.
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });

    const afterAnotherCapture = await page.evaluate(async (id) => {
      // Ended first, or the refusal below would be the one about a session already being in
      // progress — the same status code for an entirely different reason.
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
    MediaStreamTrack.prototype.getSettings = function () {
      return { ...settings.call(this), ...settled };
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
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
    await expect(page.locator('#capture')).toBeDisabled();
  } finally {
    await server.close();
    await context.close();
  }
});

test('the shutter stays taken from the press until the burst is over', async ({ browser }) => {
  // The press disabled the button and the next guidance tick put it back, because that line knew
  // only what guidance said and nothing about the arm the press had started. Nothing was armed
  // twice — `armAt` refuses a second arm while one is in flight — so what the second press got was
  // silence. A button offered while an arm is in flight lies about what pressing it does.
  //
  // The window is the time between the press and the core reporting `Firing`, and on a camera that
  // takes the locks instantly it is a few frames: traced on this runner as `capturing` at 38 ms,
  // with the button never observably back. So the camera here is slowed to open the window on
  // purpose, rather than the test hoping to land in it — a first draft passed under its own
  // sabotage for exactly that reason.
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
      return { ...settings.call(this), ...settled };
    };
    // 300 ms per constraint set, three of them: about a second of arming, well inside the three
    // the page allows one write, and long enough that a shutter put back by a guidance tick is
    // observable for many frames rather than for none.
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
    await viewfinderIsLive(page);

    // Sampled from inside the page, every animation frame, so the polling is not at the mercy of
    // the driver's round trip.
    const everEnabled = await page.evaluate(async () => {
      const button = document.getElementById('capture');
      const guidance = document.getElementById('guidance');
      button.click();
      let enabledAt = null;
      const started = performance.now();
      while (performance.now() - started < 4000) {
        await new Promise((resolve) => requestAnimationFrame(resolve));
        if (!button.disabled && enabledAt === null) enabledAt = performance.now() - started;
        if (/captured|cell done/i.test(guidance.textContent)) break;
      }
      return enabledAt;
    });
    // Null, not "some number after the burst": the claim is that there is no window at all.
    expect(everEnabled).toBeNull();
    await expect(page.locator('#guidance')).toContainText(/captured|cell done/i, { timeout: 30000 });
    // And offered again once it is over, so this cannot pass by disabling the shutter for good.
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 30000 });
  } finally {
    await server.close();
    await context.close();
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
    MediaStreamTrack.prototype.getSettings = function () {
      return { ...settings.call(this), ...settled };
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
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
    await expect(page.locator('#capture')).toBeDisabled();
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
    MediaStreamTrack.prototype.getSettings = function () {
      return { ...settings.call(this), ...settled };
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
    await viewfinderIsLive(page);

    // Three in a row, because the failure is cumulative: the first one always worked and it was
    // the second and third that were refused by a queue the first one had lengthened.
    for (let attempt = 0; attempt < 3; attempt += 1) {
      expect(await page.evaluate(() => window.sphanoramaCapture())).toBe(true);
      await expect(page.locator('#guidance'))
        .toContainText(/captured|cell done/i, { timeout: 30000 });
      await expect(page.locator('#capture')).toBeEnabled({ timeout: 30000 });
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
    MediaStreamTrack.prototype.getSettings = function () {
      return { ...settings.call(this), ...settled };
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
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
    await expect(page.locator('#capture')).toBeDisabled();
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
    MediaStreamTrack.prototype.getSettings = function () {
      return { ...settings.call(this), ...settled };
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
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
    // The camera port is real now, and nothing on this page opened a camera: the plan is sized
    // from the lens, so the session refuses rather than planning against an invented one.
    expect(outcome.startedCode).toBe('CameraUnavailable');
    expect(outcome.startedDetail).toContain('camera');
    // And a session for a project nobody created never gets as far as the camera.
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
    // And no shutter, because there is an aim: the dwell is what fires here.
    await expect(page.locator('#capture')).toBeHidden();

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

test('with no aim to hold, the button is the whole shutter', async ({ page }) => {
  // The one device the dwell cannot serve: no aim to hold, and `Stability` refuses a batch with no
  // samples rather than answering "still", so nothing can mature a dwell. The button survives there
  // and nowhere else (ADR 0043) — and on the viewfinder rather than inside the panel, which folds
  // itself once a capture starts.
  //
  // This runner is that device until an orientation event is dispatched, which is why none is.
  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText(/\d+ cells planned/, { timeout: 15000 });
    await viewfinderIsLive(page);

    await expect(page.locator('#capture')).toBeVisible({ timeout: 15000 });
    await expect(page.locator('#capture')).toBeEnabled();
    // Outside the panel, so folding the panel cannot take it away. Asserted through the DOM rather
    // than by looking at it, because "visible" is exactly what a folded panel's contents are not.
    expect(await page.evaluate(
      () => document.getElementById('capture').closest('#panel') === null)).toBe(true);

    await page.locator('#capture').click();
    await expect(page.locator('#guidance')).toContainText(/captured|cell done/i, { timeout: 15000 });
    await expect.poll(() => countCandidates(page), { timeout: 30000 }).toBe(5);
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

    await page.locator('#capture').click();
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

    await page.locator('#capture').click();
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

    await page.locator('#capture').click();
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
    await viewfinderIsLive(page);

    // One sample, so there is an attitude to place markers against, then none after the burst.
    await page.evaluate(() => {
      window.dispatchEvent(new DeviceOrientationEvent('deviceorientation', {
        alpha: 0, beta: 90, gamma: 0,
      }));
    });
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

test('a phone with no motion sensors still captures', async ({ browser }) => {
  // Declining motion on iOS lands here, and so does any desktop without sensors. The core treats
  // it as a supported configuration — PoseEngine switches to vision-only (docs/03 UC-4) — so a
  // client that refused to start a session would be inventing a restriction the core does not
  // have.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    // Both APIs, because the adapter now has two sources and a device with neither is the case
    // being described. Leaving the sensor behind would test a phone that still has one.
    delete window.AbsoluteOrientationSensor;
    delete window.DeviceOrientationEvent;
    delete window.DeviceMotionEvent;
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
    await expect(page.locator('#stage')).toContainText(/capturing without motion/, {
      timeout: 15000,
    });
    const plan = await page.evaluate(async () => {
      const got = await window.sphanoramaCore.captureSession.getPlan();
      return got.ok ? got.value.nodes.length : 0;
    });
    expect(plan).toBeGreaterThan(8);
  } finally {
    await server.close();
    await context.close();
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

    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });

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
  // On a phone producing no motion samples — which is this runner, and is the supported
  // configuration UC-4 describes — those two flags are the only true terms left in the tick gate,
  // so clearing them stops the loop asking for guidance at all and the capture is dead for good:
  // stuck part way through a burst, locks held, every further press refused. No orientation events
  // are dispatched here for exactly that reason; the round-4 test drives them and so only ever
  // exercised the case where the gate reopens by itself.
  const context = await browser.newContext();
  const page = await context.newPage();
  await page.addInitScript(() => {
    const post = Worker.prototype.postMessage;
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
      return transfer === undefined ? post.call(this, message) : post.call(this, message, transfer);
    };
  });

  const server = await serve();
  try {
    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText('capturing', { timeout: 15000 });
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
    await viewfinderIsLive(page);

    // Armed, then the very next guidance call is made to reject — so the failure lands with a
    // burst genuinely in flight, which is the state the argument is about.
    await page.evaluate(() => {
      window.__capturing = window.sphanoramaCapture();
      window.__failNextGuidance = true;
    });
    await expect.poll(() => page.evaluate(() => window.__guidanceFailuresInjected),
                      { timeout: 15000 }).toBe(1);

    // The burst finishes anyway. Without the flags surviving the rejection this never arrives:
    // no sample, no `firing`, no tick, no `AdvanceBurst`.
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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
    await viewfinderIsLive(page);

    // A burst in flight, so `firing`/`armed` are what keep the loop ticking — which is exactly the
    // state in which an unbounded hold never lets go.
    await page.evaluate(() => {
      window.__capturing = window.sphanoramaCapture();
      window.__failAllGuidance = true;
    });

    await expect(page.locator('#stage')).toContainText(/stopped answering/i, { timeout: 15000 });
    await expect(page.locator('#cell-layer .cell-ring:not([hidden])')).toHaveCount(0);
    await expect(page.locator('#capture')).toBeDisabled();

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
    await expect(page.locator('#capture')).toBeEnabled({ timeout: 15000 });
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
