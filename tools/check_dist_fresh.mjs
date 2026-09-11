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
import { spawnSync } from 'node:child_process';
import { join, dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const repoRoot = resolve(dirname(fileURLToPath(import.meta.url)), '..');

function newest(path, skip = new Set(['node_modules', '.git', 'dist', 'build']),
                accept = () => true) {
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
      if (!accept(full)) continue;
      const mtime = statSync(full).mtimeMs;
      if (mtime > latest) { latest = mtime; latestPath = full; }
    }
  };
  if (!existsSync(path)) return { mtime: 0, path };
  if (statSync(path).isDirectory()) walk(path); else latest = statSync(path).mtimeMs;
  return { mtime: latest, path: latestPath };
}

/**
 * Whether this Playwright invocation can run `bridge/test/*.spec.mjs`.
 *
 * Playwright applies its positional filters *after* `globalSetup`, so the resolved test list is not
 * available here and `process.argv` is the only description of what was asked for. Read fail-closed:
 * no positional filter means every spec is in scope, and only a filter that provably cannot match a
 * bridge spec relaxes anything. A flag like `--grep` is not a positional and does not narrow this —
 * it could select a bridge test by title.
 *
 * Deliberately not an environment variable the deploy sets. A flag that disables a check is a flag
 * somebody sets once and nobody removes; deriving it from the command means the relaxation lasts
 * exactly as long as the narrowing that earned it.
 */
function canReachBridgeSpecs(argv) {
  // Only the *first* `test` is the subcommand. Filtering every occurrence would also swallow a
  // positional filter spelled `test`, and swallowing a filter makes this answer less strict —
  // the wrong direction for a check whose whole job is to fail closed.
  const args = argv.slice(2);
  const afterSubcommand = args[0] === 'test' ? args.slice(1) : args;
  const positionals = [];
  for (let i = 0; i < afterSubcommand.length; i += 1) {
    const arg = afterSubcommand[i];
    if (!arg.startsWith('-')) { positionals.push(arg); continue; }
    // A flag taking a separate value swallows the next token, so it is not a path filter.
    if (!arg.includes('=') && afterSubcommand[i + 1] !== undefined
        && !afterSubcommand[i + 1].startsWith('-')) i += 1;
  }
  if (positionals.length === 0) return true;
  // Playwright matches a positional against the test file's path as a substring or a regular
  // expression. An unparseable pattern is treated as reaching, which is the fail-closed answer.
  const bridgeSpec = 'bridge/test/module.spec.mjs';
  return positionals.some((filter) => {
    if (bridgeSpec.includes(filter)) return true;
    try {
      return new RegExp(filter).test(bridgeSpec);
    } catch {
      return true;
    }
  });
}

/**
 * The check, against a stated root.
 *
 * Split out from the default export so the suite beside this file can build a whole fake
 * repository in a temp directory and run the real thing against it, rather than re-implementing
 * the arithmetic in a test and asserting the two agree.
 */
/**
 * The C++ translation units the wasm builds actually compile, as absolute paths — read from the
 * compile database CMake exports (`CMAKE_EXPORT_COMPILE_COMMANDS` is on repo-wide).
 *
 * `null` means there was nothing to read, and the caller then counts every source. That fallback is
 * the conservative direction on purpose: a check that quietly stops asking because a build
 * directory is missing is worse than one that asks too often.
 */
/**
 * Whether either wasm build has work it has not done — asked of ninja, which is the only thing that
 * knows.
 *
 * This is what lets a build-file edit be forgiven. An edit that is *inert* for a preset — a comment,
 * or a branch that preset does not take — reconfigures, produces no work, never relinks the core,
 * and so can never stop being newer than it: the error below would ask for a rebuild ninja
 * correctly refuses to do, which is a deadlock rather than a warning.
 *
 * An earlier version inferred this from `build.ninja`'s mtime, and a reviewer showed the inference
 * was wrong in both directions: ninja regenerates `build.ninja` *before* compiling, so a build that
 * was configured and then failed looks identical to one with nothing to do; and `CMakePresets.json`
 * is not in either preset's regeneration edge at all, so a changed preset need not rewrite it.
 * Asking is cheap and exact where guessing was neither.
 *
 * `null` means ninja could not be asked — not on PATH, no build directory, a non-zero exit — and the
 * caller then treats a build file the old way, which is the conservative direction.
 */
/**
 * When both wasm presets were last configured, as an mtime — cmake rewrites `CMakeCache.txt` on
 * every configure, so it is the record that a build file was actually *read*.
 *
 * Needed alongside ninja's answer because ninja is blind to one of the three build files:
 * `CMakePresets.json` appears in neither preset's `build.ninja`, so no edit to it can ever produce
 * outstanding work, and ninja says "nothing to do" whether the preset was configured in or not.
 * Forgiving on that alone stopped checking the presets file altogether.
 *
 * `0` when either record is missing, which forgives nothing.
 */
function lastConfiguredAt(repoRoot) {
  let earliest = 0;
  for (const preset of ['wasm-release', 'wasm-release-threaded']) {
    const cache = newest(join(repoRoot, 'build', preset, 'CMakeCache.txt'));
    if (cache.mtime === 0) return 0;
    if (earliest === 0 || cache.mtime < earliest) earliest = cache.mtime;
  }
  return earliest;
}

export function wasmBuildsAreUpToDate(repoRoot) {
  let asked = false;
  for (const preset of ['wasm-release', 'wasm-release-threaded']) {
    const dir = join(repoRoot, 'build', preset);
    if (!existsSync(join(dir, 'build.ninja'))) continue;
    const probe = spawnSync('ninja', ['-C', dir, '-n'], { encoding: 'utf8' });
    if (probe.error || probe.status !== 0) return null;
    asked = true;
    if (!`${probe.stdout}${probe.stderr}`.includes('no work to do')) return false;
  }
  return asked ? true : null;
}

function compiledTranslationUnits(repoRoot) {
  const files = new Set();
  let read = false;
  for (const preset of ['wasm-release', 'wasm-release-threaded']) {
    const db = join(repoRoot, 'build', preset, 'compile_commands.json');
    if (!existsSync(db)) continue;
    try {
      for (const entry of JSON.parse(readFileSync(db, 'utf8'))) {
        if (entry && typeof entry.file === 'string') files.add(resolve(repoRoot, entry.file));
      }
      read = true;
    } catch {
      // Unreadable or half-written: treat it as absent rather than as an empty list, which would
      // read as "the wasm build compiles nothing" and switch the check off entirely.
    }
  }
  return read ? files : null;
}

export function checkDistIsFreshIn(repoRoot, argv = process.argv,
                                   upToDateProbe = wasmBuildsAreUpToDate) {
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
  // Both builds against the same sources. The threaded one was checked for *existence* only, which
  // a reviewer pointed out buys less than it looks: a threaded core from before the change passes
  // that and its two browser tests then run against a stale one, which is the whole failure this
  // file is about, one preset over.
  const compiledCore = newest(join(coreBuild, 'sphanorama-core.wasm'));
  const compiledThreaded = newest(
    join(repoRoot, 'build', 'wasm-release-threaded', 'bridge', 'sphanorama-core.wasm'));
  if (compiledThreaded.mtime > 0 && compiledThreaded.mtime < compiledCore.mtime) {
    compiledCore.mtime = compiledThreaded.mtime;
    compiledCore.path = compiledThreaded.path;
  }
  if (compiledCore.mtime > 0) {
    // The build files are in the list too, and a reviewer had to point that out: a preset, a
    // compile flag or a source added to a `CMakeLists.txt` changes the core exactly as a `.cpp`
    // does, and three of them are outside every source directory named here.
    //
    // A `.cpp` counts only if the wasm build actually compiles it. Since ADR 0052 that is no longer
    // every source under `core/src`: `feature_registration_engine.cpp` needs OpenCV, which the wasm
    // build does not have, so it is compiled natively and nowhere else. Without this narrowing,
    // touching that file made the core stale in a way nothing could clear — ninja has no work to do
    // for a source it does not compile, so the wasm never becomes newer and the instruction this
    // error gives cannot be followed. A deadlock rather than a false alarm, which is why it is a
    // fix here and not a note in the message.
    //
    // Headers and the three CMake files are deliberately not narrowed: a header is not a
    // translation unit and never appears in a compile database, and a preset or a compile flag
    // changes the core exactly as a `.cpp` does.
    const compiled = compiledTranslationUnits(repoRoot);
    const accept = compiled === null
      ? () => true
      : (full) => !/\.(c|cc|cxx|cpp)$/.test(full) || compiled.has(full);
    const sources = ['core/src', 'bridge', 'contracts/cpp']
      .map((rel) => ({ rel, ...newest(join(repoRoot, rel), new Set(['test', 'CMakeFiles']), accept) }));

    // A build file newer than the core is forgiven only when **both** records agree: ninja has
    // nothing outstanding, *and* the configure that produced the current cache happened after the
    // edit. Ninja alone was not enough — it is blind to `CMakePresets.json` — and the cache alone is
    // not either, since a configure can be followed by a build that never ran. Together they are
    // the one case where the demand below is impossible to satisfy, which is what makes forgiving it
    // right rather than merely convenient.
    const upToDate = upToDateProbe(repoRoot);
    const configuredAt = lastConfiguredAt(repoRoot);
    const buildFiles = ['core/CMakeLists.txt', 'CMakeLists.txt', 'CMakePresets.json']
      .map((rel) => ({ rel, ...newest(join(repoRoot, rel)) }))
      .filter((s) => !(upToDate === true && configuredAt > 0 && s.mtime <= configuredAt));

    const cxx = [...sources, ...buildFiles].filter((s) => s.mtime > compiledCore.mtime);
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
  //
  // The threaded one is required only of a run that can reach the specs loading it, and that is
  // not a softening — it is the same rule read exactly. The demand exists because a *silent skip*
  // is worse than a failure; a run those specs are filtered out of has no skip to be silent about.
  // The deploy workflow is the case: it builds `wasm-release` alone, because that is the only core
  // `stage_core.mjs` publishes and the threaded one hangs without the COOP/COEP headers Pages
  // cannot serve (ADR 0011), and then verifies the bundle with `playwright test shell/e2e`. This
  // check refused it and the first deployment after it landed failed — an artifact nothing in the
  // run loads and nothing in the deploy publishes, demanded of a job that deliberately has none.
  for (const [preset, why] of [
    ['wasm-release', 'four browser tests load it directly, and every check above compares against it'],
    ['wasm-release-threaded', 'two browser tests load it directly'],
  ]) {
    if (preset === 'wasm-release-threaded' && !canReachBridgeSpecs(argv)) continue;
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

/** Playwright's `globalSetup`: the same check, against this repository. */
export default function checkDistIsFresh() {
  checkDistIsFreshIn(repoRoot, process.argv);
}
