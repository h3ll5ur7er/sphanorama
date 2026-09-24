// The back/forward cache, which the rest of the suite cannot see: Playwright launches Chromium with
// `--disable-back-forward-cache`, and its headless shell never restores a page at all. So this file
// runs the full Chromium build with the cache switched back on (ADR 0063).
import { test, expect } from '@playwright/test';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';

import { startServer } from '../../tools/static_server.mjs';

const dist = resolve(dirname(fileURLToPath(import.meta.url)), '../../dist');
const basePath = process.env.SPHANORAMA_BASE ?? '/';

test.skip(!existsSync(dist), 'run: npm run build');
test.use({
  channel: 'chromium',
  launchOptions: {
    ignoreDefaultArgs: ['--disable-back-forward-cache', '--disable-field-trial-config'],
    args: ['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream'],
  },
});

test('a page restored from the back/forward cache takes the right to its spill tier back', async ({ page }) => {
  // It lets the right go on the way out, which is what lets it be cached at all — a page holding a
  // Web Lock never is — and a page back from the cache with the worker that holds the files must
  // hold the right again, or the next newcomer would be let at a pair that is not free.
  const server = await startServer({ roots: [dist], basePath });
  try {
    await page.addInitScript(() => {
      window.__shows = [];
      addEventListener('pageshow', (event) => window.__shows.push(event.persisted));
    });
    const holders = () => page.evaluate(async () =>
      (await navigator.locks.query()).held.filter((lock) => lock.name === 'sphanorama-spill-resident').length);

    await page.goto(server.appUrl);
    await expect(page.locator('#stage')).toContainText('core ready', { timeout: 15000 });
    expect(await holders()).toBe(1);

    await page.goto(new URL('sw.js', server.appUrl).href);
    expect(await holders()).toBe(0);
    await page.goBack({ waitUntil: 'commit' });

    await expect.poll(() => page.evaluate(() => window.__shows)).toEqual([false, true]);
    await expect.poll(holders).toBe(1);
    expect(await page.evaluate(() => sessionStorage.getItem('sphanorama-resident-tier'))).toBeNull();
  } finally {
    await server.close();
  }
});
