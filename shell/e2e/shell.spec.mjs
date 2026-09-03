// End-to-end against the built bundle in dist/, not the dev server: what ships is what is tested.
//
// Chromium's fake media device stands in for a camera, so the enable path runs for real rather
// than being mocked out at exactly the point where it usually breaks.
import { test, expect } from '@playwright/test';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';

import { startServer } from '../../tools/static_server.mjs';

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

    // Five frames at 80 ms, so the burst needs about half a second of ticks to fill.
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

    await expect(page.locator('#motion-state')).toHaveText('unavailable');
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
