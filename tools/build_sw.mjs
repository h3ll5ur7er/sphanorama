// Finishes the service worker against the build it will actually ship with.
//
// Two things sw.js cannot know from source: what the assets are called (Vite hashes them, and the
// WASM core is staged in by tools/stage_core.mjs) and which build this is. Both have to be
// stamped in after the bundle exists, or the worker caches nothing on install and serves the
// first build it ever saw for as long as sw.js stays byte-identical.
import { readFile, writeFile, readdir, stat } from 'node:fs/promises';
import { createHash } from 'node:crypto';
import { dirname, join, relative, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const dist = resolve(repoRoot, 'dist');
const base = process.env.SPHANORAMA_BASE ?? '/';

// Everything the app needs to start with no network. Source maps and the worker itself are not
// on it: one is a debugging aid and the other is what is doing the caching.
const PRECACHE_EXTENSIONS = ['.html', '.css', '.js', '.wasm', '.webmanifest', '.png', '.svg'];
const EXCLUDED = new Set(['sw.js']);

async function walk(directory) {
  const found = [];
  for (const entry of await readdir(directory)) {
    const full = join(directory, entry);
    if ((await stat(full)).isDirectory()) found.push(...await walk(full));
    else found.push(full);
  }
  return found;
}

const files = (await walk(dist))
  .map((file) => relative(dist, file).split('\\').join('/'))
  .filter((name) => !EXCLUDED.has(name))
  .filter((name) => !name.endsWith('.map'))
  .filter((name) => PRECACHE_EXTENSIONS.some((extension) => name.endsWith(extension)))
  .sort();

if (files.length === 0) {
  console.error(`nothing to precache in ${dist} — run the build first`);
  process.exit(1);
}

// The build id is a hash of the bytes, not a timestamp or a counter: two builds of the same
// source produce the same id, so a redeploy that changed nothing does not evict a warm cache,
// and any deploy that changed something gets a new one for free.
const digest = createHash('sha256');
for (const name of files) {
  digest.update(name);
  digest.update(await readFile(join(dist, name)));
}
const buildId = digest.digest('hex').slice(0, 16);

const urls = files.map((name) => `${base}${name}`.replace(/\/{2,}/g, '/'));

const source = await readFile(join(dist, 'sw.js'), 'utf8');
for (const placeholder of ['__BUILD_ID__', '__PRECACHE__']) {
  if (!source.includes(placeholder)) {
    // A silently unsubstituted placeholder ships a worker that caches the literal string
    // "__PRECACHE__" and never updates. Better to fail the build.
    console.error(`sw.js has no ${placeholder} to replace — did the template change?`);
    process.exit(1);
  }
}

await writeFile(join(dist, 'sw.js'), source
  .replace('__BUILD_ID__', buildId)
  .replace('__PRECACHE__', JSON.stringify(urls, null, 2)));

console.log(`sealed sw.js: build ${buildId}, ${urls.length} precached entries`);
