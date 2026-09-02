// Minimal offline shell. Deliberately hand-written rather than generated: the whole policy is
// "serve the app shell from cache, fall back to network", and a build-time SW generator would be
// more configuration than code for that.
//
// The cache name carries a version so a deploy replaces the old shell instead of serving it
// forever — the classic PWA failure where users are pinned to a build from months ago.
const CACHE = 'sphanorama-shell-v1';

self.addEventListener('install', (event) => {
  self.skipWaiting();
  event.waitUntil(caches.open(CACHE));
});

self.addEventListener('activate', (event) => {
  event.waitUntil((async () => {
    const names = await caches.keys();
    await Promise.all(names.filter((n) => n !== CACHE).map((n) => caches.delete(n)));
    await self.clients.claim();
  })());
});

self.addEventListener('fetch', (event) => {
  const request = event.request;
  if (request.method !== 'GET' || new URL(request.url).origin !== self.location.origin) return;

  event.respondWith((async () => {
    const cached = await caches.match(request);
    if (cached) return cached;
    try {
      const response = await fetch(request);
      if (response.ok) {
        const cache = await caches.open(CACHE);
        cache.put(request, response.clone());
      }
      return response;
    } catch (cause) {
      // A cache miss while offline is a real failure; returning a fake success would leave the
      // page looking broken with no explanation.
      if (cached) return cached;
      throw cause;
    }
  })());
});
