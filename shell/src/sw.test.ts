// @vitest-environment node
// The service worker's install, run against fakes. It is a classic script with two placeholders
// `tools/build_sw.mjs` fills, so it is filled the same way here and evaluated with a stand-in
// `self` rather than imported.
import { describe, expect, it } from 'vitest';
import script from '../public/sw.js?raw';

function installed(precache: string[]): Promise<Request[]> {
  const source = script
    .replace('__BUILD_ID__', 'test')
    .replace('__PRECACHE__', JSON.stringify(precache));
  const handlers = new Map<string, (event: unknown) => void>();
  let added: Request[] = [];
  const fakeSelf = {
    addEventListener: (type: string, handler: (event: unknown) => void) => {
      handlers.set(type, handler);
    },
    skipWaiting: () => {},
  };
  const fakeCaches = {
    open: async () => ({
      addAll: async (requests: (Request | string)[]) => {
        added = requests.map((r) => (typeof r === 'string' ? new Request(r) : r));
      },
    }),
  };
  new Function('self', 'caches', source)(fakeSelf, fakeCaches);
  let waited: Promise<unknown> = Promise.resolve();
  handlers.get('install')?.({ waitUntil: (p: Promise<unknown>) => { waited = p; } });
  return waited.then(() => added);
}

describe('the service worker install', () => {
  it('precaches from the network, not from the HTTP cache', async () => {
    // The core's URL is the same in every deploy. Fetched through the HTTP cache, a new worker
    // installing within the old core's freshness lifetime stores the *old* core under the new
    // cache name, and every reload after that pairs the new page with it again (ADR 0074).
    const requests = await installed(['https://example.test/core/sphanorama-core.wasm']);
    expect(requests).toHaveLength(1);
    expect(requests[0].cache).toBe('reload');
  });
});
