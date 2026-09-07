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
import { mkdtempSync, mkdirSync, writeFileSync, rmSync, utimesSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { afterEach, describe, expect, it } from 'vitest';

import { checkDistIsFreshIn } from './check_dist_fresh.mjs';

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

/** What the check said, or null when it was happy. */
function complaint(root) {
  try {
    checkDistIsFreshIn(root);
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
    for (const test of ['core/test/a_test.cpp', 'bridge/test/b_test.cpp']) {
      const tree = aFreshTree();
      tree.put(test, Date.now());
      expect(complaint(tree.root), test).toBeNull();
    }
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
