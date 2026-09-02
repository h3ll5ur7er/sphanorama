// Copies the built WASM core into the shell's static assets.
//
// The core is an artifact of the C++ build, not a source file, so it is staged rather than
// committed. Which build gets staged is a deployment decision (ADR 0011): the single-threaded one
// by default, because GitHub Pages cannot serve the headers the threaded build needs.
import { cp, mkdir, access } from 'node:fs/promises';
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

await mkdir(to, { recursive: true });
for (const name of ['sphanorama-core.js', 'sphanorama-core.wasm']) {
  await cp(resolve(from, name), resolve(to, name));
}
console.log(`staged ${profile} core into shell/public/core`);
