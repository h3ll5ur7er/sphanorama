// Copies the built WASM core into the shell's static assets.
//
// The core is an artifact of the C++ build, not a source file, so it is staged rather than
// committed. Which build gets staged is a deployment decision (ADR 0011): the single-threaded one
// by default, because GitHub Pages cannot serve the headers the threaded build needs.
import { cp, mkdir, access, readdir } from 'node:fs/promises';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const profile = process.env.SPHANORAMA_CORE_PROFILE ?? 'wasm-release';
const from = resolve(repoRoot, `build/${profile}/bridge`);
const to = resolve(repoRoot, 'shell/public/core');

try {
  await access(from);
} catch {
  console.error(
    `no core build at ${from}\n` +
    `build it first:\n  source ~/emsdk/emsdk_env.sh\n  cmake --preset ${profile} && cmake --build build/${profile}`);
  process.exit(1);
}

// Everything the link step emitted for the module, not a list of the two files it happens to
// produce today. Emscripten's output set moves with the toolchain and the profile: pthread builds
// once shipped a separate sphanorama-core.worker.js (6.0.9 spawns the worker from the module
// itself instead), and a debug build adds a .wasm.map. A hard-coded pair silently leaves any of
// those behind, and the shortfall shows up as a 404 at runtime rather than as a failed build.
const emitted = (await readdir(from))
  .filter((name) => name.startsWith('sphanorama-core.') && !name.endsWith('.a'));

if (!emitted.includes('sphanorama-core.js') || !emitted.includes('sphanorama-core.wasm')) {
  console.error(`${from} has no linked module — expected sphanorama-core.js and .wasm, found: ` +
                `${emitted.join(', ') || '(nothing)'}`);
  process.exit(1);
}

await mkdir(to, { recursive: true });
for (const name of emitted) {
  await cp(resolve(from, name), resolve(to, name));
}
console.log(`staged ${profile} core into shell/public/core: ${emitted.join(', ')}`);
