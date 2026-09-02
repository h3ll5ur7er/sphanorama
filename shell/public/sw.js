// Offline shell. Deliberately hand-written rather than pulled from a generator: the whole policy
// is two rules, and a build-time SW toolchain would be more configuration than code for that.
//
// It is *finished* at build time, though. tools/build_sw.mjs replaces the two placeholders below
// with the real asset list and a hash of their contents, because neither can be known from here:
// Vite's filenames carry content hashes and the WASM core is staged in by a separate step. A
// service worker that cannot name what it caches cannot cache anything on install, and one whose
// cache name never changes serves the build it first saw forever.
const CACHE = 'sphanorama-shell-__BUILD_ID__';
const PRECACHE = __PRECACHE__;

// Every cache this app has ever made shares this prefix. Nothing else on the origin does — which
// matters on GitHub Pages, where every project site of an account shares one origin and deleting
// "all caches except mine" would take out someone else's app.
const CACHE_PREFIX = 'sphanorama-shell-';

self.addEventListener('install', (event) => {
  self.skipWaiting();
  // The first visit is not controlled by this worker, so its requests never reach the fetch
  // handler below. If install does not fetch the shell itself, going offline after that visit
  // finds an empty cache — which is how a PWA advertises offline support and does not have it.
  event.waitUntil(caches.open(CACHE).then((cache) => cache.addAll(PRECACHE)));
});

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    const names = await caches.keys();
    await Promise.all(names
      .filter((name) => name.startsWith(CACHE_PREFIX) && name !== CACHE)
      .map((name) => caches.delete(name)));
    await self.clients.claim();
  })());
});

self.addEventListener('fetch', (event) => {
  const request = event.request;
  if (request.method !== 'GET' || new URL(request.url).origin !== self.location.origin) return;

  event.respondWith((async () => {
    const cache = await caches.open(CACHE);

    // A navigation goes to the network first. The precached entries are versioned with the cache
    // name so they are never stale, but this is the request that decides whether a user who has
    // the app open sees this deploy or the last one.
    if (request.mode === 'navigate') {
      try {
        const response = await fetch(request);
        if (response.ok) await cache.put(request, response.clone());
        return response;
      } catch (cause) {
        const cached = await cache.match(request) ?? await cache.match('index.html');
        if (cached) return cached;
        throw cause;
      }
    }

    const cached = await cache.match(request);
    if (cached) return cached;
    const response = await fetch(request);
    // Awaited: respondWith can settle and the worker be terminated before an unawaited put has
    // written anything, so the entry silently never lands.
    if (response.ok) await cache.put(request, response.clone());
    return response;
  })());
});
