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
