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

async function serve() {
  return startServer({ roots: [dist] });
}

test('loads the core and reports its capabilities', async ({ page }) => {
  const server = await serve();
  try {
    await page.goto(server.origin);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    // The default deployment has no threads (ADR 0011). Showing it makes a slow build
    // diagnosable from a screenshot.
    await expect(page.locator('#core-caps')).toContainText('single-threaded');
    await expect(page.locator('#core-caps')).toContainText('SIMD');
  } finally {
    await server.close();
  }
});

test('enabling starts the camera and the viewfinder gets a stream', async ({ page }) => {
  const server = await serve();
  try {
    await page.goto(server.origin);
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
    await page.goto(server.origin);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    await page.locator('#enable').click();
    await expect(page.locator('#stage')).toContainText(/permission/i, { timeout: 15000 });
    await expect(page.locator('#camera-state')).toHaveText('unavailable');
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
    await page.goto(server.origin);
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
    await page.goto(server.origin);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    const outcome = await page.evaluate(async () => {
      const core = window.sphanoramaCore;
      const refused = await core.project.create('');          // an untitled project is refused
      const started = await core.captureSession.begin(1, {
        strategy: 'Rings', horizontalFovDeg: 0, verticalFovDeg: 0, overlapTarget: 0.3,
        acceptanceConeDeg: 4, coverPoles: true, motion: 'None',
      });
      return {
        refusedCode: refused.ok ? null : refused.status.code,
        startedCode: started.ok ? null : started.status.code,
        startedDetail: started.ok ? null : started.status.detail,
      };
    });
    expect(outcome.refusedCode).toBe('InvalidArgument');
    // No camera port exists, so the session refuses with a reason rather than producing frames.
    expect(outcome.startedCode).toBe('CameraUnavailable');
    expect(outcome.startedDetail).toContain('port');
  } finally {
    await server.close();
  }
});

test('registers a service worker so the shell works offline', async ({ page }) => {
  const server = await serve();
  try {
    await page.goto(server.origin);
    // Registration is deliberately not awaited by the app — offline support must never delay or
    // block first paint — so the test polls rather than assuming it has already happened.
    const registered = await page
      .waitForFunction(async () => (await navigator.serviceWorker.getRegistration()) !== undefined,
                       null, { timeout: 15000 })
      .then(() => true)
      .catch(() => false);
    expect(registered).toBe(true);
  } finally {
    await server.close();
  }
});
