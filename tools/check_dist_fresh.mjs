/**
 * Refuses to run the browser suite against a bundle older than the sources it was built from.
 *
 * The suite serves `dist/` itself rather than through Playwright's `webServer`, so nothing in the
 * run knows whether that directory was built from the code under test. `npm run build` typechecks
 * first and leaves the previous `dist` in place when it fails — so a run can be green against a
 * bundle from an hour ago, and the failure it hides is the one the change was making. A reviewer
 * hit exactly that twice in one round: two false-green sabotage runs, one of them after
 * `npm run build` had exited 2 on a typecheck error.
 *
 * The gate reads `npm run build`'s exit status and catches it there. This catches it for anyone
 * running `npx playwright test` directly, which is what a person debugging one test actually does.
 *
 * Modification times rather than hashes, because the question is only "was this built after the
 * code changed" and a fresh checkout has no `dist` at all — the missing case is the loud one.
 */
import { readdirSync, statSync, existsSync, readFileSync } from 'node:fs';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');

function newest(path, skip = new Set(['node_modules', '.git', 'dist', 'build'])) {
  let latest = 0;
  let latestPath = path;
  const walk = (at) => {
    let entries;
    try {
      entries = readdirSync(at, { withFileTypes: true });
    } catch {
      return;   // vanished mid-walk, or unreadable; not this check's business
    }
    for (const entry of entries) {
      if (skip.has(entry.name)) continue;
      const full = join(at, entry.name);
      if (entry.isDirectory()) { walk(full); continue; }
      const mtime = statSync(full).mtimeMs;
      if (mtime > latest) { latest = mtime; latestPath = full; }
    }
  };
  if (!existsSync(path)) return { mtime: 0, path };
  if (statSync(path).isDirectory()) walk(path); else latest = statSync(path).mtimeMs;
  return { mtime: latest, path: latestPath };
}

export default function checkDistIsFresh() {
  const dist = join(repoRoot, 'dist');
  if (!existsSync(dist)) {
    throw new Error(
      'dist/ does not exist — the browser suite serves it, so there is nothing to test.\n' +
      'Run `npm run build` in shell/ first (and read its exit status).');
  }

  const built = newest(dist);
  // What the bundle is built from. The wasm is staged by `npm run build` rather than compiled by
  // it, so `shell/public/core` is in the list: a core rebuilt and not staged is the other half of
  // this trap, and the one the skill's own lens text calls out.
  const sources = ['shell/src', 'shell/index.html', 'contracts/ts', 'shell/public']
    .map((rel) => ({ rel, ...newest(join(repoRoot, rel)) }))
    .filter((s) => s.mtime > 0);

  // The wasm is *staged* rather than compiled by `npm run build`, so comparing mtimes against
  // `shell/public` catches a core that was staged late and misses one that was rebuilt and never
  // staged at all — which is the same trap one step earlier, and the one a reviewer walked into
  // while sabotaging the core itself. Content rather than mtime, because that question has an
  // exact answer and a timestamp only has a plausible one.
  //
  // Both halves of the core, and the glue is not a formality: every `EM_JS` body in `bridge/`
  // compiles into `sphanorama-core.js`, not into the wasm. So the whole camera and motion port —
  // `host_camera_metric`'s switch included, which is the seam an e2e test exists to pin — can be
  // changed with the `.wasm` coming out byte for byte identical. A reviewer sabotaged `case 8`,
  // rebuilt, and this check had nothing to say.
  const coreBuild = join(repoRoot, 'build', 'wasm-release', 'bridge');
  const coreStage = join(repoRoot, 'shell', 'public', 'core');
  for (const file of ['sphanorama-core.wasm', 'sphanorama-core.js']) {
    const compiled = join(coreBuild, file);
    const staged = join(coreStage, file);
    if (existsSync(compiled) && existsSync(staged)
        && !readFileSync(compiled).equals(readFileSync(staged))) {
      throw new Error(
        `the compiled core and the staged one are different files (${file}) — the browser suite\n` +
        'would test a core that is not the one you just built.\n' +
        `  compiled: ${compiled}\n` +
        `  staged:   ${staged}\n` +
        'Run `npm run build` in shell/ (and read its exit status).');
    }
  }

  // And the core against the C++ it is built from, which nothing above asks about: a compiled core
  // that was never rebuilt matches the staged one exactly, so every comparison up to here is happy
  // while the browser runs a core from before the change. The same trap as the bundle's, one
  // language further down.
  //
  // `core/test` and `bridge/test` are left out on purpose — they are not linked into the wasm, and
  // demanding a five-minute rebuild for a native test edit would teach people to skip this check.
  const compiledCore = newest(join(coreBuild, 'sphanorama-core.wasm'));
  if (compiledCore.mtime > 0) {
    // The build files are in the list too, and a reviewer had to point that out: a preset, a
    // compile flag or a source added to a `CMakeLists.txt` changes the core exactly as a `.cpp`
    // does, and three of them are outside every source directory named here.
    const cxx = ['core/src', 'bridge', 'contracts/cpp',
                 'core/CMakeLists.txt', 'CMakeLists.txt', 'CMakePresets.json']
      .map((rel) => ({ rel, ...newest(join(repoRoot, rel), new Set(['test', 'CMakeFiles'])) }))
      .filter((s) => s.mtime > compiledCore.mtime);
    if (cxx.length > 0) {
      const worst = cxx.reduce((a, b) => (a.mtime > b.mtime ? a : b));
      throw new Error(
        'the compiled core is older than the C++ it is built from — the browser suite would test a\n' +
        'core from before your change and pass.\n' +
        `  newest source: ${worst.path}\n` +
        `  compiled core: ${compiledCore.path}\n` +
        'Run `tools/gate.sh` (or the wasm build), then `npm run build` in shell/ to stage it.');
    }
  }

  // Both wasm builds, whose absence is silent rather than red: `bridge/test/module.spec.mjs` loads
  // each directly and *skips* its tests when the directory is not there, so a run that never built
  // one reports "4 skipped" among sixty passes and nobody reads that as a gap. The gate builds both
  // presets; a person running Playwright alone builds neither.
  //
  // The single-threaded one was left out of the first version of this check, and a reviewer showed
  // it costs more than four skipped tests: everything above — the compiled-against-staged
  // comparison and the whole C++-staleness block — is conditioned on that file existing, so its
  // absence quietly disables them too. It is checked here rather than there so the message names
  // the build to run instead of describing what could not be compared.
  for (const [preset, why] of [
    ['wasm-release', 'four browser tests load it directly, and every check above compares against it'],
    ['wasm-release-threaded', 'two browser tests load it directly'],
  ]) {
    const built = join(repoRoot, 'build', preset, 'bridge', 'sphanorama-core.wasm');
    if (!existsSync(built)) {
      throw new Error(
        `the ${preset} wasm build is missing — ${why}, and what is missing skips\n` +
        'silently rather than failing.\n' +
        `  expected: ${built}\n` +
        `Run \`cmake --preset ${preset} && cmake --build build/${preset}\`, or \`tools/gate.sh\`,\n` +
        'which builds both.');
    }
  }

  const stale = sources.filter((s) => s.mtime > built.mtime);
  if (stale.length > 0) {
    const worst = stale.reduce((a, b) => (a.mtime > b.mtime ? a : b));
    throw new Error(
      `dist/ is older than the sources it is built from — the browser suite would test a stale\n` +
      `bundle and pass. Newest source: ${worst.path}\n` +
      `Newest built file: ${built.path}\n` +
      'Run `npm run build` in shell/ (and read its exit status: it typechecks first and leaves\n' +
      'the previous dist in place when that fails).');
  }
}
