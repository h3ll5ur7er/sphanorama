// @vitest-environment node
// The service worker's install, run against fakes. It is a classic script with two placeholders
// `tools/build_sw.mjs` fills, so it is filled the same way here and evaluated with a stand-in
// `self` rather than imported.
import { describe, expect, it } from 'vitest';
import script from '../public/sw.js?raw';

interface Installing {
  // What install handed the cache, once it has.
  requests: () => Request[];
  // Whether the promise install handed `waitUntil` has settled.
  settled: () => boolean;
  // Lets the cache's `addAll` finish.
  fill: () => void;
}

async function installing(precache: string[]): Promise<Installing> {
  const source = script
    .replace('__BUILD_ID__', 'test')
    .replace('__PRECACHE__', JSON.stringify(precache));
  const handlers = new Map<string, (event: unknown) => void>();
  let added: Request[] = [];
  let fill = () => {};
  const filled = new Promise<void>((resolve) => { fill = resolve; });
  const fakeSelf = {
    addEventListener: (type: string, handler: (event: unknown) => void) => {
      handlers.set(type, handler);
    },
    skipWaiting: () => {},
  };
  const fakeCaches = {
    open: async () => ({
      addAll: (requests: (Request | string)[]) => {
        added = requests.map((r) => (typeof r === 'string' ? new Request(r) : r));
        return filled;
      },
    }),
  };
  new Function('self', 'caches', source)(fakeSelf, fakeCaches);
  let settled = false;
  handlers.get('install')?.({
    waitUntil: (p: Promise<unknown>) => { void p.finally(() => { settled = true; }); },
  });
  // Long enough for `caches.open` to answer and `addAll` to be called.
  for (let i = 0; i < 10; i += 1) await Promise.resolve();
  return { requests: () => added, settled: () => settled, fill };
}

describe('the service worker install', () => {
  it('precaches every entry from the network, not from the HTTP cache', async () => {
    // The core's two files and the scope root have the same URLs in every deploy. Fetched through
    // the HTTP cache, a new worker installing within the old ones' freshness lifetime stores them
    // under the new cache name, and every launch after that pairs the new page with the old core
    // or the old page with the new one (ADR 0074). Every entry, because any one of them is enough.
    const precache = [
      'https://example.test/',
      'https://example.test/core/sphanorama-core.js',
      'https://example.test/core/sphanorama-core.wasm',
      'https://example.test/index.html',
    ];
    const install = await installing(precache);
    expect(install.requests().map((r) => r.url)).toEqual(precache);
    for (const request of install.requests()) expect(request.cache).toBe('reload');
  });

  it('is not done until the cache is filled', async () => {
    // A worker that activates before its cache holds the shell claims the page and then has
    // nothing to answer it with offline.
    const install = await installing(['https://example.test/index.html']);
    expect(install.requests()).toHaveLength(1);
    expect(install.settled()).toBe(false);
    install.fill();
    for (let i = 0; i < 10; i += 1) await Promise.resolve();
    expect(install.settled()).toBe(true);
  });
});
