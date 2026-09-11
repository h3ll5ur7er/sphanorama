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
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, rmSync, utimesSync, chmodSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { afterEach, describe, expect, it } from 'vitest';

import { checkDistIsFreshIn, expandPresetMacros, kEnvironmentDependent, kUnknownMacro,
         wasmBuildsAreUpToDate } from './check_dist_fresh.mjs';

const made = [];
afterEach(() => {
  for (const dir of made.splice(0)) rmSync(dir, { recursive: true, force: true });
  // Set by the real-shaped preset fixture, which needs an environment variable to expand.
  delete process.env.SPHANORAMA_TEST_SDK;
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
      // Respects `-C` the way real ninja does — it exits 1 on a directory with no manifest — so a
      // sabotage that deletes the existence check is not silently answered by the shim instead.
      writeFileSync(shim, `#!/bin/sh\n[ -f "$2/build.ninja" ] || exit 1\n${script}\n`);
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

  it('runs the real probe when none is injected, in the direction that forgives', () => {
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

  it('runs the real probe when none is injected, in the direction that refuses', () => {
    // Both directions, because one case cannot pin a binding. The forgiving case above dies to a
    // default of `() => null` or `() => false`; this one dies to `() => true` — the "production
    // forgives everything" mutant, which is the one an earlier version of the case above was written
    // to catch and did not. A binding needs a test on each side of it or it has a blind spot
    // whichever way you point the single case.
    // The manifests stay in place so the graph set is populated — that is what makes this
    // discriminating. Real ninja refuses the fixture's stub manifest and exits non-zero, so the real
    // probe answers "cannot ask" and nothing is forgiven, while a default of `() => true` would
    // forgive the file on a graph it never checked.
    const tree = aFreshTree();
    tree.put('core/CMakeLists.txt', Date.now());
    let message = null;
    try { checkDistIsFreshIn(tree.root, ['node', 'playwright', 'test']); } catch (e) { message = e.message; }
    expect(message).toMatch(/compiled core is older than the C\+\+/);
  });

  it('does not forgive a preset whose generator or toolchain changed', () => {
    // Neither is a `cacheVariable`, so comparing only those ignored two fields that decide what
    // gets built — and a preset carrying a `toolchainFile` is exactly how the wasm builds are
    // configured here.
    for (const [field, value] of [['generator', 'Unix Makefiles'], ['toolchainFile', '/elsewhere.cmake']]) {
      const tree = aFreshTree();
      const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
      presets.configurePresets[0][field] = value;
      tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
      expect(complaint(tree.root, undefined, () => true), field)
        .toMatch(/compiled core is older than the C\+\+/);
    }
  });

  it('does not forgive a preset carrying a field this checker has never heard of', () => {
    // The rule that keeps the others honest. A preset key nobody here knows — a newer CMake's, or
    // one simply missed — refuses forgiveness rather than being skipped, so falling behind CMake
    // makes this ask too often instead of quietly stopping.
    const tree = aFreshTree();
    const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
    presets.configurePresets[0].someFutureCMakeField = { that: 'changes the build' };
    tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
    expect(complaint(tree.root, undefined, () => true)).toMatch(/compiled core is older than the C\+\+/);
  });

  it('reads the variables a preset inherits, not only the ones it restates', () => {
    // `inherits` is how three of this repository's six presets are written. Reading only a preset's
    // own `cacheVariables` compared fewer things than the configure used, which forgives too
    // readily — the unsafe direction.
    const tree = aFreshTree();
    const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
    presets.configurePresets.push({ name: 'base', cacheVariables: { CMAKE_CXX_FLAGS: '-msimd128' } });
    presets.configurePresets[0] = { name: 'wasm-release', inherits: 'base' };
    tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
    expect(complaint(tree.root, undefined, () => true)).toBeNull();

    const changed = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
    changed.configurePresets.find((p) => p.name === 'base').cacheVariables.CMAKE_CXX_FLAGS = '-O0';
    tree.put('CMakePresets.json', Date.now(), JSON.stringify(changed));
    expect(complaint(tree.root, undefined, () => true)).toMatch(/compiled core is older than the C\+\+/);
  });

  it('reads a cache variable written the documented {type, value} way', () => {
    // CMake documents both forms. Stringifying the object gave `[object Object]`, which matched
    // nothing — so a preset written the documented way could never be forgiven at all.
    const tree = aFreshTree();
    const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
    presets.configurePresets[0].cacheVariables.CMAKE_CXX_FLAGS = { type: 'STRING', value: '-msimd128' };
    tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
    expect(complaint(tree.root, undefined, () => true)).toBeNull();
  });


  describe('presets shaped the way this repository writes them', () => {
    // Every case above is written against `{ name, cacheVariables }` — a shape none of the six real
    // presets has. That is not a cosmetic gap. Three mutants survived all 44 tests: dropping
    // `displayName` from the documentation-only set, dropping `binaryDir` from the handled-elsewhere
    // set, and emptying the compared-against-the-cache map entirely. Each one makes forgiveness
    // permanently unavailable for every preset in this repository — the exact failure this function
    // exists to avoid — and none of them can be caught by a case that asserts a *refusal*, because a
    // refusal is what they all produce. What catches them is one positive case on a preset carrying
    // the fields the real ones carry.

    /** This repository's own two wasm presets, field for field, with a cache that agrees. */
    const realShaped = (tree) => {
      process.env.SPHANORAMA_TEST_SDK = '/opt/sdk';
      const t = Date.now() - 99000;
      const toolchain = '/opt/sdk/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake';
      tree.put('CMakePresets.json', t, JSON.stringify({
        configurePresets: [
          {
            name: 'wasm-release',
            displayName: 'WASM, single-threaded — the build GitHub Pages can serve',
            binaryDir: '${sourceDir}/build/wasm-release',
            generator: 'Ninja',
            toolchainFile:
              '$env{SPHANORAMA_TEST_SDK}/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake',
            cacheVariables: { CMAKE_BUILD_TYPE: 'Release', CMAKE_CXX_FLAGS: '-msimd128 -Oz -flto' },
          },
          {
            name: 'wasm-release-threaded',
            inherits: 'wasm-release',
            displayName: 'WASM, threaded — needs a host that can serve COOP/COEP',
            binaryDir: '${sourceDir}/build/wasm-release-threaded',
            cacheVariables: { CMAKE_CXX_FLAGS: '-msimd128 -pthread -Oz -flto' },
          },
        ],
      }));
      for (const [preset, flags] of [['wasm-release', '-msimd128 -Oz -flto'],
                                     ['wasm-release-threaded', '-msimd128 -pthread -Oz -flto']]) {
        tree.put(`build/${preset}/CMakeCache.txt`, t,
                 'CMAKE_BUILD_TYPE:STRING=Release\n'
                 + `CMAKE_CXX_FLAGS:STRING=${flags}\n`
                 + 'CMAKE_GENERATOR:INTERNAL=Ninja\n'
                 + `CMAKE_TOOLCHAIN_FILE:FILEPATH=${toolchain}\n`);
      }
      return tree;
    };

    it('forgives a documentation-only edit to one of them', () => {
      // The case that was missing, and the one that fails without macro expansion: `toolchainFile`
      // is `$env{EMSDK}/…` in both real presets and the cache holds the path CMake resolved it to,
      // so comparing the two as text matched nothing. The forgiveness these four fixes were written
      // to provide was unreachable in this repository from the commit that added it, while the fake
      // tree above — which has no macro anywhere in it — stayed green.
      //
      // It also pins *nearest declaration winning*: `wasm-release-threaded` overrides the flags it
      // inherits, so reading the base's value instead compares `-Oz -flto` against a cache holding
      // `-pthread -Oz -flto` and refuses.
      const tree = realShaped(aFreshTree());
      const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
      presets.configurePresets[0].displayName = 'a better sentence about the same build';
      tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
      expect(complaint(tree.root, undefined, () => true)).toBeNull();
    });

    it('still refuses one of them whose toolchain macro resolves somewhere else', () => {
      // The other half of the case above: expansion must compare the resolved path, not skip the
      // field. Same preset, same cache, one different environment.
      const tree = realShaped(aFreshTree());
      process.env.SPHANORAMA_TEST_SDK = '/opt/some-other-sdk';
      tree.put('CMakePresets.json', Date.now(),
               readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
      expect(complaint(tree.root, undefined, () => true))
        .toMatch(/compiled core is older than the C\+\+/);
    });

    it('refuses one of them carrying a macro this checker cannot resolve', () => {
      // The unknown-key rule, one level down. A macro nobody here writes is a value this checker
      // cannot compare, so it refuses rather than comparing the unexpanded text — which is precisely
      // the mistake that made `$env{}` unforgivable.
      //
      // The cache is given the macro's own text, so the unknown macro is the *only* thing standing
      // between this preset and forgiveness. The first version of this case put the macro in the
      // preset alone: the values then disagreed for an ordinary reason, it refused for that reason
      // instead, and a sabotage that made the expander never refuse left it green.
      const tree = realShaped(aFreshTree());
      const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
      presets.configurePresets[0].cacheVariables.CMAKE_CXX_FLAGS = '-msimd128 ${hostSystemName}';
      tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
      tree.put('build/wasm-release/CMakeCache.txt', Date.now() - 99000,
               'CMAKE_BUILD_TYPE:STRING=Release\n'
               + 'CMAKE_CXX_FLAGS:STRING=-msimd128 ${hostSystemName}\n'
               + 'CMAKE_GENERATOR:INTERNAL=Ninja\n'
               + 'CMAKE_TOOLCHAIN_FILE:FILEPATH=/opt/sdk/upstream/emscripten/cmake/Modules/'
               + 'Platform/Emscripten.cmake\n');
      expect(complaint(tree.root, undefined, () => true))
        .toMatch(/compiled core is older than the C\+\+/);
    });

    it('forgives a base preset marked hidden', () => {
      // `hidden` is CMake's documented way to say "this one is not for direct use", which is how a
      // base preset is written. Refusing it as an unknown key killed forgiveness for every preset
      // that inherits from one — and the unknown-key rule is worth keeping precisely because it is
      // this easy to be wrong about, so the answer is to know the key rather than to soften the rule.
      const tree = realShaped(aFreshTree());
      const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
      presets.configurePresets.push({
        name: 'wasm-base', hidden: true, cacheVariables: { CMAKE_BUILD_TYPE: 'Release' },
      });
      presets.configurePresets[0].inherits = 'wasm-base';
      delete presets.configurePresets[0].cacheVariables.CMAKE_BUILD_TYPE;
      tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
      expect(complaint(tree.root, undefined, () => true)).toBeNull();
    });

    it('reads a base preset that two parents both inherit', () => {
      // A diamond is legal CMake and was read as a cycle: the visited set was shared across sibling
      // parents, so the second branch to reach a shared base was told it had already been there. A
      // cycle is a name repeating on one *path*, not a name repeating anywhere in the walk.
      const tree = realShaped(aFreshTree());
      const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
      presets.configurePresets.push(
        { name: 'wasm-base', cacheVariables: { CMAKE_BUILD_TYPE: 'Release' } },
        { name: 'wasm-left', inherits: 'wasm-base' },
        { name: 'wasm-right', inherits: 'wasm-base' });
      presets.configurePresets[0].inherits = ['wasm-left', 'wasm-right'];
      delete presets.configurePresets[0].cacheVariables.CMAKE_BUILD_TYPE;
      tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
      expect(complaint(tree.root, undefined, () => true)).toBeNull();
    });

    it('reads every preset named in an array of inherits, not just the first', () => {
      // The array form is what the diamond above is written with, so a checker that collapsed it to
      // its first element would pass that case while reading half the declarations — forgiving on
      // the strength of variables it never compared.
      const tree = realShaped(aFreshTree());
      const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
      presets.configurePresets.push(
        { name: 'wasm-first', cacheVariables: { CMAKE_BUILD_TYPE: 'Release' } },
        { name: 'wasm-second', cacheVariables: { CMAKE_CXX_FLAGS: 'not what the cache holds' } });
      presets.configurePresets[0].inherits = ['wasm-first', 'wasm-second'];
      delete presets.configurePresets[0].cacheVariables.CMAKE_CXX_FLAGS;
      tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
      expect(complaint(tree.root, undefined, () => true))
        .toMatch(/compiled core is older than the C\+\+/);
    });

    it('still forgives a documentation-only edit when the toolchain variable is unset', () => {
      // The deadlock the macro expansion re-entered while fixing. With `SPHANORAMA_TEST_SDK` unset
      // there is no value to compare `toolchainFile` against — and refusing on that basis produced
      // a complaint nothing could clear, because an inert preset edit gives the build no work to do
      // and so never changes the core's mtime. `gate.sh` sources `emsdk_env.sh` and never sees it;
      // running the browser suite directly does, which is the case this whole file is written for.
      const tree = realShaped(aFreshTree());
      delete process.env.SPHANORAMA_TEST_SDK;
      const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
      presets.configurePresets[0].displayName = 'a better sentence about the same build';
      tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
      expect(complaint(tree.root, undefined, () => true)).toBeNull();
    });

    it('still refuses a real flag change when the toolchain variable is unset', () => {
      // The other half: skipping the variable this environment cannot resolve must not skip the
      // ones it can. Everything except `toolchainFile` is still compared.
      const tree = realShaped(aFreshTree());
      delete process.env.SPHANORAMA_TEST_SDK;
      const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
      presets.configurePresets[0].cacheVariables.CMAKE_CXX_FLAGS = '-msimd128 -O0';
      tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
      expect(complaint(tree.root, undefined, () => true))
        .toMatch(/compiled core is older than the C\+\+/);
    });

    it('inherits a field from the first preset in the list that defines it', () => {
      // CMake's documented precedence for the array form, and the opposite of what folding the
      // parents in order gives. Both parents here declare the same variable and only the first
      // agrees with the cache, so applying them front to back would refuse a preset CMake configures
      // exactly as this cache records.
      const tree = realShaped(aFreshTree());
      const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
      presets.configurePresets.push(
        { name: 'wasm-first', cacheVariables: { CMAKE_BUILD_TYPE: 'Release' } },
        { name: 'wasm-second', cacheVariables: { CMAKE_BUILD_TYPE: 'Debug' } });
      presets.configurePresets[0].inherits = ['wasm-first', 'wasm-second'];
      delete presets.configurePresets[0].cacheVariables.CMAKE_BUILD_TYPE;
      tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
      expect(complaint(tree.root, undefined, () => true)).toBeNull();
    });

    it('does not forgive a preset that configures a different directory', () => {
      // `binaryDir` was filed "handled elsewhere" and handled nowhere. A preset that builds
      // somewhere else declares nothing about the cache this function reads, so its agreement would
      // be a coincidence.
      const tree = realShaped(aFreshTree());
      const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
      presets.configurePresets[0].binaryDir = '${sourceDir}/build/somewhere-else';
      tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
      expect(complaint(tree.root, undefined, () => true))
        .toMatch(/compiled core is older than the C\+\+/);
    });

    it('can resolve every macro this repository actually writes into its presets', () => {
      // The canary the fake trees cannot be: a macro added to the real file that this checker does
      // not know would make forgiveness unavailable again, and every case above would stay green
      // because none of them reads that file. This one does.
      // Found by walking up rather than resolved against `import.meta.url`, which vitest's
      // transform does not leave as a `file:` URL, and rather than against the working directory,
      // which would make this the one case in the suite that fails when launched from elsewhere.
      let root = process.cwd();
      while (!existsSync(join(root, 'CMakePresets.json')) && dirname(root) !== root) root = dirname(root);
      const text = readFileSync(join(root, 'CMakePresets.json'), 'utf8');
      const macros = [...text.matchAll(/\$[A-Za-z]*\{[^}]*\}/g)].map((m) => m[0]);
      expect(macros.length).toBeGreaterThan(0);
      for (const macro of macros) {
        const resolved = expandPresetMacros(macro, { presetName: 'wasm-release', repoRoot: '/r' });
        // Never "I do not know this macro". It may legitimately be "this environment holds no value
        // for it" — `$env{EMSDK}` is exactly that when the caller has not sourced `emsdk_env.sh` —
        // and that answer is handled rather than refused, which is what stops it deadlocking.
        expect(resolved, macro).not.toBe(kUnknownMacro);
        if (resolved !== kEnvironmentDependent) {
          expect(resolved, macro).not.toMatch(/\$[A-Za-z]*\{/);
        }
      }
    });
  });

  it('does not forgive a preset that inherits in a circle', () => {
    const tree = aFreshTree();
    const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
    presets.configurePresets[0] = { name: 'wasm-release', inherits: 'loop' };
    presets.configurePresets.push({ name: 'loop', inherits: 'wasm-release' });
    tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
    expect(complaint(tree.root, undefined, () => true)).toMatch(/compiled core is older than the C\+\+/);
  });

  it('does not forgive a preset that declares a variable the build directory has never held', () => {
    // A newly added cache variable is a configure that has not happened. Treating "not in the cache"
    // as agreement forgave exactly that.
    const tree = aFreshTree();
    const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
    presets.configurePresets[0].cacheVariables.SPHANORAMA_NEW_KNOB = 'ON';
    tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
    expect(complaint(tree.root, undefined, () => true)).toMatch(/compiled core is older than the C\+\+/);
  });

  it('does not forgive a preset the build directories do not name at all', () => {
    // A preset removed or renamed while its build directory still exists: nothing declares what that
    // directory holds, so nothing can vouch for it.
    const tree = aFreshTree();
    const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
    presets.configurePresets = presets.configurePresets.filter((p) => p.name !== 'wasm-release');
    tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
    expect(complaint(tree.root, undefined, () => true)).toMatch(/compiled core is older than the C\+\+/);
  });

  it('does not read a nested build directory as a source', () => {
    // `newest`'s default skip list is `node_modules`, `.git`, `dist`, `build`. The source walk needed
    // to skip two *more* directories and passed its own set, which replaced those four rather than
    // adding to them — so anything generated under a walked root counted as a source, and a file the
    // build itself writes is always newer than the core built before it. That is the permanently-red
    // shape this checker has twice been fixed for: nothing a developer does can make it green.
    for (const buried of ['bridge/node_modules/pkg/generated.cpp',
                          'bridge/build/scratch/generated.cpp',
                          'core/src/dist/generated.cpp']) {
      const tree = aFreshTree();
      tree.put(buried, Date.now());
      expect(complaint(tree.root, undefined, () => true), buried).toBeNull();
    }
  });

  it('does not demand a wasm graph name a CMakeLists the wasm build excludes', () => {
    // The wasm presets set `SPHANORAMA_BUILD_TESTS=OFF`, so `core/test/CMakeLists.txt` appears in no
    // wasm build graph — measured on the real tree: 0 mentions in `build/wasm-release/build.ninja`
    // against 3 in the native one. Walking for every `CMakeLists.txt` therefore filed it as
    // permanently suspect, and the printed remedy could not clear it: editing it gives the wasm
    // build nothing to do, so the core is never relinked and the complaint outlives the change.
    const tree = aFreshTree();
    tree.put('core/test/CMakeLists.txt', Date.now());
    expect(complaint(tree.root, undefined, () => true)).toBeNull();
  });

  it('checks a CMakeLists nobody wrote down', () => {
    // The list was written out four times and was one short every time. This one is in a directory
    // the source walk covers and is named by no build graph, so both the walk and the build-file
    // rule have to agree it is theirs — which, when they did not, made it invisible to each.
    const tree = aFreshTree();
    tree.put('bridge/resource_access/CMakeLists.txt', Date.now());
    expect(complaint(tree.root, undefined, () => true)).toMatch(/compiled core is older than the C\+\+/);
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
    //
    // The preset stays valid JSON on purpose. An earlier version wrote the literal string
    // `CMakePresets.json` into it, so `JSON.parse` refused it before the probe was consulted and the
    // case passed without ever reaching what it was about.
    const tree = aFreshTree();
    const presets = JSON.parse(readFileSync(join(tree.root, 'CMakePresets.json'), 'utf8'));
    tree.put('CMakePresets.json', Date.now(), JSON.stringify(presets));
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
                        ['node', 'playwright', 'test', 'test', 'shell/e2e'],
                        // Playwright matches a positional as a substring *or* a regular
                        // expression, and every case above is a substring — so the regex arm and
                        // its `catch` were reached by nothing in this suite. Two mutants proved it:
                        // matching by substring alone, and a `catch` returning `false`, each left
                        // all 53 tests green while flipping these three from reaching to not
                        // reaching. Every flip drops the threaded demand for a run that does load
                        // `bridge/test/module.spec.mjs`, whose absence then skips silently — which
                        // is the failure this whole file exists to refuse, arriving through the one
                        // function written to fail closed.
                        ['node', 'playwright', 'test', 'bridge/test/.*spec'],
                        ['node', 'playwright', 'test', 'bridge.*module'],
                        // Unparseable, so `new RegExp` throws. "Reaching" is the fail-closed answer
                        // to a filter this cannot understand.
                        ['node', 'playwright', 'test', 'bridge/test/(unclosed']]) {
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
