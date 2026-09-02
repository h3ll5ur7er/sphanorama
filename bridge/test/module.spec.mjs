// Runs the shipped WASM module in a real browser, served the way the deployment target serves it.
//
// This is the only honest test of a `-sENVIRONMENT=web,worker` artifact: node cannot load it, and
// building a node-flavoured variant would mean testing something other than what ships.
import { test, expect } from '@playwright/test';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';
import { existsSync } from 'node:fs';

import { startServer } from '../../tools/static_server.mjs';

const here = dirname(fileURLToPath(import.meta.url));
const repoRoot = resolve(here, '../..');
const fixtures = here;

const builds = {
  singleThreaded: resolve(repoRoot, 'build/wasm-release/bridge'),
  threaded: resolve(repoRoot, 'build/wasm-release-threaded/bridge'),
};

async function load(page, { build, crossOriginIsolated }) {
  const server = await startServer({ roots: [build, fixtures], crossOriginIsolated });
  try {
    await page.goto(`${server.origin}/harness.html`);
    await page.waitForFunction(() => window.sphanorama !== undefined, null, { timeout: 15000 });
    return await page.evaluate(() => ({
      ready: window.sphanorama.ready,
      error: window.sphanorama.error ?? null,
      isolated: window.sphanorama.crossOriginIsolated ?? null,
      probeUnisolated: window.sphanorama.ready ? window.sphanorama.probe(8, false) : null,
      probeIsolated: window.sphanorama.ready ? window.sphanorama.probe(8, true) : null,
    }));
  } finally {
    await server.close();
  }
}

test.describe('single-threaded core — the build GitHub Pages can serve', () => {
  test.skip(!existsSync(builds.singleThreaded), 'run: cmake --build build/wasm-release');

  test('loads without cross-origin isolation', async ({ page }) => {
    const result = await load(page, { build: builds.singleThreaded, crossOriginIsolated: false });
    expect(result.error).toBeNull();
    expect(result.ready).toBe(true);
    expect(result.isolated).toBe(false);
  });

  test('never claims threads it does not have', async ({ page }) => {
    const result = await load(page, { build: builds.singleThreaded, crossOriginIsolated: false });
    expect(result.probeUnisolated.threads).toBe(false);
    expect(result.probeUnisolated.sharedMemory).toBe(false);
    expect(result.probeUnisolated.hardwareConcurrency).toBe(0);
  });

  test('still reports no threads even when the host is isolated', async ({ page }) => {
    // Cross-origin isolation is necessary for threads, not sufficient: this binary has no
    // pthread runtime, and a probe answering from the host rather than the build would have
    // callers sizing a pool that cannot exist.
    const result = await load(page, { build: builds.singleThreaded, crossOriginIsolated: true });
    expect(result.isolated).toBe(true);
    expect(result.probeIsolated.threads).toBe(false);
  });

  test('has SIMD compiled in', async ({ page }) => {
    // -msimd128 is set by every preset; losing it would halve blend throughput with nothing
    // failing to announce it.
    const result = await load(page, { build: builds.singleThreaded, crossOriginIsolated: false });
    expect(result.probeUnisolated.simd).toBe(true);
  });
});

test.describe('threaded core — needs a host that can serve COOP/COEP', () => {
  test.skip(!existsSync(builds.threaded), 'run: cmake --build build/wasm-release-threaded');

  test('reports threads when the host is cross-origin isolated', async ({ page }) => {
    const result = await load(page, { build: builds.threaded, crossOriginIsolated: true });
    expect(result.error).toBeNull();
    expect(result.isolated).toBe(true);
    expect(result.probeIsolated.threads).toBe(true);
    expect(result.probeIsolated.sharedMemory).toBe(true);
    expect(result.probeIsolated.hardwareConcurrency).toBe(8);
  });

  test('does not become usable at all when served without the headers', async ({ page }) => {
    // The deployment question, answered by the artifact rather than by reasoning about it.
    //
    // A threaded build shipped to a host that cannot isolate does not degrade to "no threads" —
    // it never finishes initialising, because the runtime needs a SharedArrayBuffer that the
    // browser refuses to hand out. The page hangs with no error. This is why GitHub Pages gets
    // the single-threaded build (ADR 0011), and the assertion exists so that shipping the wrong
    // one is a red build rather than a blank screen on someone's phone.
    const server = await startServer({
      roots: [builds.threaded, fixtures],
      crossOriginIsolated: false,
    });
    try {
      await page.goto(`${server.origin}/harness.html`);
      const becameUsable = await page
        .waitForFunction(() => window.sphanorama?.ready === true, null, { timeout: 5000 })
        .then(() => true)
        .catch(() => false);
      expect(becameUsable).toBe(false);
      expect(await page.evaluate(() => self.crossOriginIsolated)).toBe(false);
    } finally {
      await server.close();
    }
  });
});
