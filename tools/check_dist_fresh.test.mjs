// The freshness gate's own tests, which it did not have.
//
// Every Python checker in this directory has a `test_*.py` that `gate.sh` runs, on the reasoning
// in that file's own header: the checkers "never change while you are working", so a broken one is
// the easiest thing not to notice. This one is JavaScript, so it is a vitest suite instead — and
// it was written after a reviewer pointed out that a new hard check, added test-last, in the one
// checker nothing checked, is the shape this file exists to complain about.
//
// Each case builds a whole fake repository in a temp directory and runs the real check against it,
// so what is asserted is the check's behaviour rather than a re-implementation of its arithmetic.
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, rmSync, utimesSync, chmodSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, describe, expect, it } from 'vitest';

import { checkDistIsFreshIn, wasmBuildsAreUpToDate } from './check_dist_fresh.mjs';

const made = [];
afterEach(() => {
  for (const dir of made.splice(0)) rmSync(dir, { recursive: true, force: true });
});

/** A repository where everything is in order, so each test can break exactly one thing. */
function aFreshTree() {
  const root = mkdtempSync(join(tmpdir(), 'fresh-'));
  made.push(root);
  const put = (rel, at, body) => {
    const path = join(root, rel);
    mkdirSync(join(path, '..'), { recursive: true });
    // The compiled core and the staged one have to be byte-identical for a *fresh* tree, since
    // that comparison is content rather than mtime — so the body defaults to the file's basename
    // rather than its path.
    writeFileSync(path, body ?? rel.slice(rel.lastIndexOf('/') + 1));
    if (at !== undefined) utimesSync(path, at / 1000, at / 1000);
    return path;
  };
  // Sources first, then the cores built from them, then the bundle staged from those: the order
  // the mtimes have to be in for the tree to be fresh.
  const t = Date.now() - 100000;
  put('core/src/a.cpp', t);
  put('bridge/b.cpp', t);
  put('contracts/cpp/c.h', t);
  put('core/CMakeLists.txt', t);
  put('CMakeLists.txt', t);
  put('CMakePresets.json', t);
  for (const preset of ['wasm-release', 'wasm-release-threaded']) {
    put(`build/${preset}/CMakeCache.txt`, t + 1000, 'CMAKE_CXX_FLAGS:STRING=-msimd128\n');
    put(`build/${preset}/build.ninja`, t + 1000,
        'build build.ninja: RERUN_CMAKE | ../../CMakeLists.txt ../../core/CMakeLists.txt '
        + '../../bridge/CMakeLists.txt\n');
  }
  put('CMakePresets.json', t, JSON.stringify({
    configurePresets: [
      { name: 'wasm-release', cacheVariables: { CMAKE_CXX_FLAGS: '-msimd128' } },
      { name: 'wasm-release-threaded', cacheVariables: { CMAKE_CXX_FLAGS: '-msimd128' } },
    ],
  }));
  put('build/wasm-release/bridge/sphanorama-core.wasm', t + 1000);
  put('build/wasm-release/bridge/sphanorama-core.js', t + 1000);
  put('build/wasm-release-threaded/bridge/sphanorama-core.wasm', t + 1000);
  put('shell/public/core/sphanorama-core.wasm', t + 2000);
  put('shell/public/core/sphanorama-core.js', t + 2000);
  put('shell/src/main.ts', t + 2000);
  put('shell/index.html', t + 2000);
  put('contracts/ts/contracts.d.ts', t + 2000);
  put('dist/index.html', t + 3000);
  return { root, put };
}

/**
 * What the check said, or null when it was happy.
 *
 * `argv` is the Playwright invocation, defaulting to one with no positional filter — every spec in
 * scope, which is the strict reading and what `gate.sh` does.
 */
function complaint(root, argv = ['node', 'playwright', 'test'], upToDate = () => null) {
  try {
    checkDistIsFreshIn(root, argv, upToDate);
    return null;
  } catch (error) {
    return error.message;
  }
}

describe('the dist freshness check', () => {
  it('says nothing about a tree where everything was built in order', () => {
    expect(complaint(aFreshTree().root)).toBeNull();
  });

  it('catches a bundle older than the sources it is built from', () => {
    const tree = aFreshTree();
    tree.put('shell/src/main.ts', Date.now());
    expect(complaint(tree.root)).toMatch(/dist\/ is older than the sources/);
  });

  it('catches a core that was rebuilt and never staged', () => {
    // Content rather than mtime, because that question has an exact answer and a timestamp only a
    // plausible one. Both halves of the core, since every host-facing body compiles into the glue
    // `.js` rather than the wasm — the seam a reviewer changed with the wasm byte-identical.
    for (const half of ['sphanorama-core.wasm', 'sphanorama-core.js']) {
      const tree = aFreshTree();
      writeFileSync(join(tree.root, 'build/wasm-release/bridge', half), 'something else');
      expect(complaint(tree.root), half).toMatch(/compiled core and the staged one are different/);
    }
  });

  it('catches a core older than the C++ it is built from, build files included', () => {
    // The three CMake files are in this list because a preset, a compile flag or a source added to
    // one changes the core exactly as a `.cpp` does, and all three sit outside every source
    // directory the check names.
    for (const source of ['core/src/a.cpp', 'bridge/b.cpp', 'contracts/cpp/c.h',
                          'core/CMakeLists.txt', 'CMakeLists.txt', 'CMakePresets.json']) {
      const tree = aFreshTree();
      tree.put(source, Date.now());
      expect(complaint(tree.root), source).toMatch(/compiled core is older than the C\+\+/);
    }
  });

  it('ignores a C++ source the wasm build does not compile', () => {
    // ADR 0052 put the first core source behind a build flag: `feature_registration_engine.cpp`
    // needs OpenCV, which the wasm build does not have, so it is compiled natively and nowhere
    // else. Before this, touching it made the core permanently stale — ninja had no work to do,
    // so the wasm could never become newer than it, and the check could not be cleared by doing
    // what it asked. A deadlock, not a false alarm.
    const tree = aFreshTree();
    tree.put('build/wasm-release/compile_commands.json', undefined,
             JSON.stringify([{ file: join(tree.root, 'core/src/a.cpp') },
                             { file: join(tree.root, 'bridge/b.cpp') }]));
    tree.put('core/src/engines/registration_engine/native_only.cpp', Date.now());
    expect(complaint(tree.root)).toBeNull();
  });

  it('still catches a C++ source the wasm build does compile', () => {
    // The other half, without which the check above is just the check switched off.
    const tree = aFreshTree();
    tree.put('build/wasm-release/compile_commands.json', undefined,
             JSON.stringify([{ file: join(tree.root, 'core/src/a.cpp') },
                             { file: join(tree.root, 'bridge/b.cpp') }]));
    tree.put('core/src/a.cpp', Date.now());
    expect(complaint(tree.root)).toMatch(/compiled core is older than the C\+\+/);
  });

  it('counts every C++ source when there is no compile database to narrow it', () => {
    // The fallback is the conservative one. A tree with no `compile_commands.json` — a different
    // generator, or a build directory that was never configured — gets the old behaviour rather
    // than a check that quietly stops asking.
    const tree = aFreshTree();
    tree.put('core/src/engines/registration_engine/native_only.cpp', Date.now());
    expect(complaint(tree.root)).toMatch(/compiled core is older than the C\+\+/);
  });

  it('keeps asking about headers and build files, which no compile database lists', () => {
    // A header is not a translation unit, so it never appears in a compile database — narrowing by
    // one must not turn the header check off. Same for the three CMake files.
    for (const source of ['contracts/cpp/c.h', 'core/CMakeLists.txt', 'CMakePresets.json']) {
      const tree = aFreshTree();
      tree.put('build/wasm-release/compile_commands.json', undefined,
               JSON.stringify([{ file: join(tree.root, 'core/src/a.cpp') }]));
      tree.put(source, Date.now());
      expect(complaint(tree.root), source).toMatch(/compiled core is older than the C\+\+/);
    }
  });

  it('falls back to counting everything when the compile database cannot be parsed', () => {
    // The `catch` in `compiledTranslationUnits` is the whole reason it returns `null` rather than
    // an empty set, and nothing reached it. A reviewer made that branch do exactly what its own
    // comment forbids — treat a half-written file as a successful read — and the suite stayed
    // green, which means the conservative fallback was a comment rather than a behaviour.
    const tree = aFreshTree();
    tree.put('build/wasm-release/compile_commands.json', undefined, '[{"file": "truncated...');
    tree.put('core/src/engines/registration_engine/native_only.cpp', Date.now());
    expect(complaint(tree.root)).toMatch(/compiled core is older than the C\+\+/);
  });

  it('reads the threaded preset\'s compile database too, not only the first one', () => {
    // Both wasm builds compile the core, and the check takes the older of the two. A reviewer
    // misspelled `wasm-release-threaded` in the preset list and the suite stayed green, because
    // every fake tree happened to carry the single-threaded database as well. This one carries
    // only the threaded database, so a list that does not read it narrows nothing and the source
    // below — which that database does list — stops being noticed.
    const tree = aFreshTree();
    tree.put('build/wasm-release-threaded/compile_commands.json', undefined,
             JSON.stringify([{ file: join(tree.root, 'core/src/a.cpp') }]));
    tree.put('core/src/engines/registration_engine/native_only.cpp', Date.now());
    expect(complaint(tree.root)).toBeNull();
  });

  // The real probe, which every case above replaces with a stub. Nothing executed it, and that is
  // precisely how a production-only regression sat behind ten green cases — the injected stub made
  // the *branch* testable and left the *function* untested, and a green suite was the proof.
  describe('the ninja probe itself', () => {
    /** A build directory with a `build.ninja` that says what we want it to say. */
    function treeWithNinja(root, script) {
      mkdirSync(join(root, 'bin'), { recursive: true });
      const shim = join(root, 'bin', 'ninja');
      writeFileSync(shim, `#!/bin/sh\n${script}\n`);
      chmodSync(shim, 0o755);
      for (const preset of ['wasm-release', 'wasm-release-threaded']) {
        mkdirSync(join(root, 'build', preset), { recursive: true });
        writeFileSync(join(root, 'build', preset, 'build.ninja'), 'rule x\n');
      }
      return shim;
    }

    function withPath(dir, run) {
      const saved = process.env.PATH;
      process.env.PATH = `${dir}:${saved}`;
      try { return run(); } finally { process.env.PATH = saved; }
    }

    it('says yes only when ninja reports nothing to do', () => {
      const root = mkdtempSync(join(tmpdir(), 'probe-'));
      made.push(root);
      const shim = treeWithNinja(root, 'echo "ninja: no work to do."');
      expect(withPath(join(shim, '..'), () => wasmBuildsAreUpToDate(root))).toBe(true);
    });

    it('says no when ninja would build something', () => {
      const root = mkdtempSync(join(tmpdir(), 'probe-'));
      made.push(root);
      const shim = treeWithNinja(root, 'echo "[1/2] Building CXX object foo.o"');
      expect(withPath(join(shim, '..'), () => wasmBuildsAreUpToDate(root))).toBe(false);
    });

    it('says nothing at all when ninja fails', () => {
      // A non-zero exit is not a licence to forgive — a broken manifest must not read as idle.
      const root = mkdtempSync(join(tmpdir(), 'probe-'));
      made.push(root);
      const shim = treeWithNinja(root, 'echo "ninja: error: loading build.ninja" >&2; exit 1');
      expect(withPath(join(shim, '..'), () => wasmBuildsAreUpToDate(root))).toBeNull();
    });

    it('says nothing at all when there is no build directory to ask about', () => {
      const root = mkdtempSync(join(tmpdir(), 'probe-'));
      made.push(root);
      expect(wasmBuildsAreUpToDate(root)).toBeNull();
    });

    it('reads ninja\'s answer on stderr as well as stdout', () => {
      // Some ninja builds print the message on stderr. Scanning only stdout would read that as
      // "there is work", which is the conservative direction and therefore silent — a branch that
      // is wrong and never complains is the kind that survives a review.
      const root = mkdtempSync(join(tmpdir(), 'probe-'));
      made.push(root);
      const shim = treeWithNinja(root, 'echo "ninja: no work to do." >&2');
      expect(withPath(join(shim, '..'), () => wasmBuildsAreUpToDate(root))).toBe(true);
    });

    it('says nothing at all when ninja is not on PATH', () => {
      // Not a separate branch from a failing ninja, which is worth saying: `spawnSync` reports a
      // missing command as `status === null`, so the same comparison catches both. The case is kept
      // because it is a state that happens to people, not because it reaches its own line.
      const root = mkdtempSync(join(tmpdir(), 'probe-'));
      made.push(root);
      treeWithNinja(root, 'echo "ninja: no work to do."');
      const saved = process.env.PATH;
      process.env.PATH = '/nonexistent-for-this-test';
      try {
        expect(wasmBuildsAreUpToDate(root)).toBeNull();
      } finally {
        process.env.PATH = saved;
      }
    });

    it('skips a preset with no build.ninja rather than failing on it', () => {
      // Both orders, because removing only the *last* preset leaves `continue` and `break`
      // indistinguishable — the loop was finished either way. With the first preset missing, a
      // `break` would skip the second and answer for neither.
      for (const missing of ['wasm-release', 'wasm-release-threaded']) {
        const root = mkdtempSync(join(tmpdir(), 'probe-'));
        made.push(root);
        const shim = treeWithNinja(root, 'echo "ninja: no work to do."');
        rmSync(join(root, 'build', missing), { recursive: true, force: true });
        expect(withPath(join(shim, '..'), () => wasmBuildsAreUpToDate(root)), missing).toBe(true);
      }
    });

    it('asks about both presets, not just the first', () => {
      // A shim that answers "idle" for the single-threaded tree and "busy" for the threaded one.
      const root = mkdtempSync(join(tmpdir(), 'probe-'));
      made.push(root);
      const shim = treeWithNinja(root,
        'case "$2" in *threaded*) echo "[1/2] Building CXX object foo.o";; *) echo "ninja: no work to do.";; esac');
      expect(withPath(join(shim, '..'), () => wasmBuildsAreUpToDate(root))).toBe(false);
    });
  });

  it('forgives every CMakeLists the build graph names, not a list someone wrote down', () => {
    // `bridge/CMakeLists.txt` is in every preset's regeneration edge exactly as the other two are,
    // and an enumeration naming only two left it deadlocking — the fourth enumeration on this branch
    // to come up one short. The set is read out of `build.ninja` now, so a CMakeLists added anywhere
    // is covered without anyone remembering to add it here.
    for (const source of ['core/CMakeLists.txt', 'CMakeLists.txt', 'bridge/CMakeLists.txt']) {
      const tree = aFreshTree();
      tree.put(source, Date.now());
      expect(complaint(tree.root, undefined, () => true), source).toBeNull();
    }
  });

  it('forgives a preset edit that changes no cache variable', () => {
    // `displayName` and `description` are documentation-only preset fields — one of this repo's
    // presets carries 700 characters of prose — so "JSON has no comments, therefore no inert edit"
    // was wrong. And there is no mtime that answers here: `cmake --preset` rewrites `CMakeCache.txt`
    // only when a value changes, measured over three consecutive no-op runs. What answers is the
    // content: the preset's declared cache variables against the ones the build directory holds.
    const tree = aFreshTree();
    const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
    presets.configurePresets[0].description = 'a newly written explanation, changing no build';
    tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
    expect(complaint(tree.root, undefined, () => true)).toBeNull();
  });

  it('does not forgive a preset edit that changes a cache variable', () => {
    // The half that matters: a flag change the build directory has not been reconfigured for.
    const tree = aFreshTree();
    const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
    presets.configurePresets[0].cacheVariables.CMAKE_CXX_FLAGS = '-msimd128 -O0';
    tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
    expect(complaint(tree.root, undefined, () => true)).toMatch(/compiled core is older than the C\+\+/);
  });

  it('never forgives CMakePresets.json, however idle ninja is', () => {
    // Ninja is blind to this one file: it appears in neither wasm `build.ninja` (measured, `grep -c`
    // is 0 in both), so no edit to it can produce outstanding work and ninja answers "nothing to do"
    // whether the preset was configured in or not. Forgiving on that answer silently stopped
    // checking the only build file whose changes ninja cannot see.
    //
    // It is not forgiven at all rather than forgiven on some other evidence, because the deadlock
    // forgiveness exists for cannot arise here: `CMakePresets.json` is JSON, and JSON has no
    // comments, so there is no such thing as an edit to it that is inert for a build.
    // Still true when the preset's *content* has moved: ninja is blind to this file, so its answer
    // says nothing about it either way.
    const tree = aFreshTree();
    const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
    presets.configurePresets[1].cacheVariables.CMAKE_CXX_FLAGS = '-msimd128 -pthread';
    tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
    expect(complaint(tree.root, undefined, () => true)).toMatch(/compiled core is older than the C\+\+/);
  });

  it('runs the real probe when none is injected', () => {
    // The blind spot that hid a production regression for a whole round: every other case here
    // passes a stub, so the default binding was reached by nothing and a mutant making production
    // forgive everything stayed green.
    //
    // It has to exercise the **forgiving** direction to pin that binding. An earlier version put a
    // build file in a tree with no build directory and asserted a complaint, which a default of
    // `() => null` *or* `() => false` satisfies just as well — it pinned "the default refuses",
    // which is not the same claim. Here the tree has a ninja shim that reports idle, so only the
    // real probe can produce the forgiveness this asserts.
    const tree = aFreshTree();
    mkdirSync(join(tree.root, 'bin'), { recursive: true });
    const shim = join(tree.root, 'bin', 'ninja');
    writeFileSync(shim, '#!/bin/sh\necho "ninja: no work to do."\n');
    chmodSync(shim, 0o755);
    tree.put('core/CMakeLists.txt', Date.now());

    const saved = process.env.PATH;
    process.env.PATH = `${join(tree.root, 'bin')}:${saved}`;
    try {
      let message = null;
      try { checkDistIsFreshIn(tree.root, ['node', 'playwright', 'test']); } catch (e) { message = e.message; }
      expect(message).toBeNull();
    } finally {
      process.env.PATH = saved;
    }
  });

  it('forgives a build file when ninja says there is nothing left to do', () => {
    // A `CMakeLists.txt` edit that is inert for a preset — a comment, or a branch that preset does
    // not take — reconfigures and produces no work, so the core is never relinked and can never
    // become newer than the file. That deadlocked the gate: the error's own instruction ("run the
    // wasm build, then stage it") cannot clear it, because ninja correctly has nothing to do.
    // The two `CMakeLists.txt` files only, because they are the ones ninja can answer for: both
    // appear in each preset's regeneration edge, so "nothing to do" means cmake re-ran and found
    // nothing, rather than meaning nobody asked.
    for (const source of ['core/CMakeLists.txt', 'CMakeLists.txt']) {
      const tree = aFreshTree();
      tree.put(source, Date.now());
      expect(complaint(tree.root, undefined, () => true), source).toBeNull();
    }
  });

  it('does not forgive a build file when ninja still has work pending', () => {
    // The hole the first version of this had. Ninja regenerates `build.ninja` *before* it compiles,
    // so a build that was configured and then failed reaches exactly the state the old mtime
    // inference read as "absorbed" — and a failed build followed by a browser run is the whole
    // scenario this file exists to refuse.
    for (const source of ['core/CMakeLists.txt', 'CMakeLists.txt']) {
      const tree = aFreshTree();
      tree.put(source, Date.now());
      expect(complaint(tree.root, undefined, () => false), source)
        .toMatch(/compiled core is older than the C\+\+/);
    }
  });

  it('does not forgive a build file when ninja could not be asked', () => {
    // No answer is not a yes. Missing build directory, no ninja on PATH, a non-zero exit: the
    // conservative comparison stands, which is the behaviour every other case here assumes.
    const tree = aFreshTree();
    tree.put('CMakePresets.json', Date.now());
    expect(complaint(tree.root, undefined, () => null)).toMatch(/compiled core is older than the C\+\+/);
  });

  it('asks ninja only about build files, never about sources', () => {
    // The forgiveness is scoped. A `.cpp` the wasm build compiles is stale whatever ninja says
    // about outstanding work, because a source newer than the core is exactly the thing this check
    // is for — and an inert *source* edit is not a thing.
    const tree = aFreshTree();
    tree.put('core/src/a.cpp', Date.now());
    expect(complaint(tree.root, undefined, () => true)).toMatch(/compiled core is older than the C\+\+/);
  });

  it('catches a threaded core older than the C++, not only the single-threaded one', () => {
    // Existence alone was what this build got at first, which a reviewer pointed out buys less
    // than it looks: a threaded core from before the change passes that, and its two browser tests
    // then run against a stale one.
    const tree = aFreshTree();
    const t = Date.now() - 200000;
    tree.put('build/wasm-release-threaded/bridge/sphanorama-core.wasm', t);
    expect(complaint(tree.root)).toMatch(/compiled core is older than the C\+\+/);
  });

  it('catches either wasm build missing, because what is missing skips silently', () => {
    // `bridge/test/module.spec.mjs` loads each build directly and *skips* its tests when the
    // directory is not there, so a run that never built one reports "4 skipped" among sixty passes.
    // And a missing single-threaded build takes the two checks above down with it, since both are
    // conditioned on that file existing.
    for (const preset of ['wasm-release', 'wasm-release-threaded']) {
      const tree = aFreshTree();
      rmSync(join(tree.root, 'build', preset), { recursive: true });
      expect(complaint(tree.root), preset).toMatch(new RegExp(`the ${preset} wasm build is missing`));
    }
  });

  it('does not demand a rebuild for a C++ test edit', () => {
    // `core/test` and `bridge/test` are excluded because they are not linked into the wasm, and
    // the reason is the file's own: demanding a five-minute rebuild for a native test edit would
    // teach people to skip the check. That rule had no case, which a reviewer pointed out is
    // exactly the shape this suite exists for — the exclusion is what keeps the checker usable, so
    // it is the line most costly to lose quietly.
    //
    // Only one of the two cases is load-bearing, and a later reviewer measured which: `bridge/test`
    // sits *inside* a walked root, so the `test` entry in the skip set is the only thing excluding
    // it and deleting that entry fails this test. `core/test` is excluded twice over — the root is
    // `core/src` rather than `core`, and the skip set would catch it even if that were widened —
    // so it survives either sabotage alone and pins nothing by itself. Kept anyway, and said out
    // loud rather than left to look like coverage it does not provide: it is the case that fails
    // if both are ever lost together, which is what a refactor of the root list would do.
    for (const test of ['core/test/a_test.cpp', 'bridge/test/b_test.cpp']) {
      const tree = aFreshTree();
      tree.put(test, Date.now());
      expect(complaint(tree.root), test).toBeNull();
    }
  });

  it('does not demand the threaded core for a run that cannot reach the specs loading it', () => {
    // The deploy workflow's case, and the one that broke the first deployment after this check
    // landed. It builds `wasm-release` alone — deliberately, because that is the only core
    // `stage_core.mjs` publishes and the threaded one hangs without COOP/COEP, which Pages cannot
    // serve (ADR 0011) — and then verifies the bundle with `npx playwright test shell/e2e`.
    //
    // The requirement below is about `bridge/test/module.spec.mjs`, which loads each build
    // directly and skips silently when one is absent. A run that cannot reach that file cannot
    // suffer that silence, so demanding the build costs a two-minute compile of an artifact
    // nothing in the run loads and nothing in the deploy publishes.
    const tree = aFreshTree();
    rmSync(join(tree.root, 'build', 'wasm-release-threaded'), { recursive: true });
    expect(complaint(tree.root, ['node', 'playwright', 'test', 'shell/e2e'])).toBeNull();
  });

  it('still demands it when the filter can reach those specs', () => {
    // Fail closed, which is what makes the case above safe: only a filter that provably cannot
    // match `bridge/test/module.spec.mjs` relaxes anything. Everything else — no filter at all, a
    // filter naming the bridge suite, a filter naming a single test by title — keeps the demand.
    for (const argv of [['node', 'playwright', 'test'],
                        ['node', 'playwright', 'test', 'bridge'],
                        ['node', 'playwright', 'test', 'bridge/test/module.spec.mjs'],
                        ['node', 'playwright', 'test', '--grep', 'threads'],
                        // A positional spelled like the subcommand, *beside* one that could not
                        // reach on its own. `test` is a substring of `bridge/test/module.spec.mjs`
                        // so the run does reach it — and an earlier version that dropped every
                        // `test` token swallowed the filter and answered "cannot reach", which is
                        // the one direction a fail-closed check must not err in. The second filter
                        // is what makes the two spellings distinguishable: with the list emptied
                        // entirely, "no filter" also answers "reaching" and hides the difference.
                        ['node', 'playwright', 'test', 'test', 'shell/e2e']]) {
      const tree = aFreshTree();
      rmSync(join(tree.root, 'build', 'wasm-release-threaded'), { recursive: true });
      expect(complaint(tree.root, argv), argv.join(' '))
        .toMatch(/the wasm-release-threaded wasm build is missing/);
    }
  });

  it('always demands the single-threaded core, whatever the filter', () => {
    // Not symmetric with the pair above, and the asymmetry is the point: `wasm-release` is the
    // build the bundle is staged from, so every check above compares against it and every run
    // needs it — the deploy included, which is why the deploy builds it.
    const tree = aFreshTree();
    rmSync(join(tree.root, 'build', 'wasm-release'), { recursive: true });
    expect(complaint(tree.root, ['node', 'playwright', 'test', 'shell/e2e']))
      .toMatch(/the wasm-release wasm build is missing/);
  });

  it('catches a bundle older than each of the things it is built from', () => {
    // Four source roots feed the bundle and only `shell/src` was driven. `contracts/ts` in
    // particular is generated from the C++ headers, so a contract change that reaches the browser
    // suite through a stale bundle is the exact failure this file was written for.
    for (const source of ['shell/src/main.ts', 'shell/index.html', 'contracts/ts/contracts.d.ts',
                          'shell/public/core/sphanorama-core.js']) {
      const tree = aFreshTree();
      tree.put(source, Date.now(), 'sphanorama-core.js');
      expect(complaint(tree.root), source).toMatch(/dist\/ is older than the sources/);
    }
  });

  it('says the loud thing when there is no bundle at all', () => {
    const tree = aFreshTree();
    rmSync(join(tree.root, 'dist'), { recursive: true });
    expect(complaint(tree.root)).toMatch(/dist\/ does not exist/);
  });
});
