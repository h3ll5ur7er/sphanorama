// The WebAssembly accuracy step's own tests, which it did not have.
//
// `wasm_accuracy.sh` decides whether the gate's and CI's "wasm accuracy" step is green, and the
// real runner always answers cleanly, so none of its refusals was ever exercised: a reviewer
// reverted a round's fix to them and nothing noticed. Each case here stands a fake runner where the
// script looks for one and runs the real script against it. The fake answers a tiny ring, which
// renders in well under a second.
import { spawnSync } from 'node:child_process';
import { chmodSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { afterEach, describe, expect, it } from 'vitest';

const repoRoot = join(dirname(fileURLToPath(import.meta.url)), '..');
const panorama = 'core/test/data/panoramas/small_hangar_01_1k.jpg';

const made = [];
afterEach(() => {
  while (made.length) rmSync(made.pop(), { recursive: true, force: true });
});

/**
 * A build directory whose runner answers as told. `solved` is how many `[wasm-solved]` lines a
 * measurement prints and `exit` its status; `detectors` and `ring` are the raw answers, printed
 * as they are, so a banner line or a stray word is the runner's rather than this helper's.
 */
function buildDir({ detectors = '3\n', ring = `2 16 12 ${panorama}\n`, solved = 3, exit = 0,
                    detectorsExit = 0, ringExit = 0 } = {}) {
  const dir = mkdtempSync(join(tmpdir(), 'wasm-accuracy-'));
  made.push(dir);
  mkdirSync(join(dir, 'bin'));
  const runner = join(dir, 'bin', 'sphanorama_wasm_accuracy.cjs');
  writeFileSync(runner, `
    const arg = process.argv[2];
    if (arg === '--detectors') { process.stdout.write(${JSON.stringify(detectors)}); process.exit(${detectorsExit}); }
    if (arg === '--ring') { process.stdout.write(${JSON.stringify(ring)}); process.exit(${ringExit}); }
    // Measures only what was rendered, so a script that skipped the render cannot pass.
    if (!require('node:fs').existsSync(require('node:path').join(arg, 'truth.json'))) {
      console.log('FAIL: no rendered ring at ' + arg);
      process.exit(1);
    }
    for (let i = 0; i < ${solved}; i += 1) console.log('[wasm-solved] detector=' + i);
    process.exit(${exit});
  `);
  chmodSync(runner, 0o644);
  return dir;
}

function run(...dirs) {
  return runFrom(repoRoot, ...dirs);
}

function runFrom(cwd, ...dirs) {
  return runWith({ cwd }, ...dirs);
}

function runWith({ cwd = repoRoot, env = process.env, script = join(repoRoot, 'tools', 'wasm_accuracy.sh') },
                 ...dirs) {
  // A hang is a failure: a ring too large to render took the step with it. Through `timeout`, which
  // signals the whole process group, because a timeout on `bash` alone left the renderer it had
  // started running after the test had failed.
  const result = spawnSync('timeout', ['-k', '5', '60', 'bash', script, ...dirs],
                           { cwd, env, encoding: 'utf8' });
  return { status: result.status, output: `${result.stdout}${result.stderr}` };
}

describe('the WebAssembly accuracy step', () => {
  it('passes a runner that measured every detector it has', () => {
    const { status, output } = run(buildDir());
    expect(output).toMatch(/\[wasm-solved\] detector=2/);
    expect(status).toBe(0);
  });

  it('passes from outside the repository root, where the panorama path does not resolve', () => {
    // The panorama `--ring` names is relative to the repository root, which is where the native
    // renderer runs its generator from; the script has to find it from anywhere it is called.
    const elsewhere = mkdtempSync(join(tmpdir(), 'elsewhere-'));
    made.push(elsewhere);
    const { status, output } = runFrom(elsewhere, buildDir());
    expect(output).toMatch(/\[wasm-solved\] detector=2/);
    expect(status).toBe(0);
  });

  it('passes with CDPATH exported, which makes `cd` print where it went', () => {
    // Called by a relative path, as the gate calls it, since CDPATH is consulted only for those, and
    // with a CDPATH entry holding a `tools/` of its own, which `cd tools/..` would otherwise go to.
    const decoy = mkdtempSync(join(tmpdir(), 'decoy-'));
    made.push(decoy);
    mkdirSync(join(decoy, 'tools'));
    const { status, output } = runWith({ env: { ...process.env, CDPATH: decoy }, script: 'tools/wasm_accuracy.sh' },
                                       buildDir());
    expect(output).toMatch(/\[wasm-solved\] detector=2/);
    expect(status).toBe(0);
  });

  it('stops when it cannot make a scratch directory, rather than rendering at the root', () => {
    const { status, output } = runWith({ env: { ...process.env, TMPDIR: join(tmpdir(), 'nowhere-at-all') } },
                                       buildDir());
    expect(output).toMatch(/could not make a scratch directory/);
    expect(status).toBe(2);
  });

  it('names a build directory with no runner rather than skipping it', () => {
    const dir = mkdtempSync(join(tmpdir(), 'wasm-accuracy-'));
    made.push(dir);
    const { status, output } = run(dir);
    expect(output).toMatch(/the build did not produce the measurement/);
    expect(status).toBe(1);
  });

  it('fails on any answer to --detectors that is not a count of at least one', () => {
    // The twenty digits are the case `[` cannot compare, and it answers that with status 2 — which
    // an unnegated `[ … -lt 1 ]` read as "not less than one" and so as a pass.
    // `+3` and ` 3` are the ones `[` accepts, so `count` alone refuses them.
    for (const detectors of ['99999999999999999999\n', '0\n', '3 detectors\n', '3\nnote\n', '', '-3\n',
                             '+3\n', ' 3\n']) {
      const { status, output } = run(buildDir({ detectors, solved: 0 }));
      expect(output, JSON.stringify(detectors)).toMatch(/--detectors did not answer with a count/);
      expect(status, JSON.stringify(detectors)).toBe(1);
    }
  });

  it('fails when asking a question fails, whatever was printed', () => {
    // Each question on its own, since a failing first question hides the second.
    const detectors = run(buildDir({ detectorsExit: 1 }));
    expect(detectors.output).toMatch(/--detectors did not answer/);
    expect(detectors.status).toBe(1);
    const ring = run(buildDir({ ringExit: 1 }));
    expect(ring.output).toMatch(/--ring did not answer/);
    expect(ring.status).toBe(1);
  });

  it('fails when the ring does not render, rather than measuring nothing', () => {
    // A file that is not an image passes every check on the answer's shape and is refused by the
    // renderer.
    const { status, output } = run(buildDir({ ring: '2 16 12 README.md\n' }));
    expect(output).toMatch(/could not be read as an image/);
    expect(output).toMatch(/the ring did not render, so nothing was measured/);
    expect(status).toBe(1);
  });

  it('fails on any answer to --ring that is not a shape and a panorama that exists', () => {
    for (const ring of ['2 16 12\n', '2 16 12 core/test/data/panoramas/nowhere.jpg\n',
                        `2 16 12 ${panorama} extra\n`, `2 16 12 ${panorama}\nnote\n`,
                        `two 16 12 ${panorama}\n`, `2 sixteen 12 ${panorama}\n`,
                        `2 16 twelve ${panorama}\n`, `0 16 12 ${panorama}\n`,
                        `99999999999999999999 16 12 ${panorama}\n`,
                        // A sign, which `[` accepts and only `count` refuses.
                        `+2 16 12 ${panorama}\n`, `2 +16 12 ${panorama}\n`, `2 16 +12 ${panorama}\n`,
                        `2 99999999999999999999 12 ${panorama}\n`, `2 16 99999999999999999999 ${panorama}\n`,
                        `2 0 12 ${panorama}\n`, `2 16 0 ${panorama}\n`, '']) {
      const { status, output } = run(buildDir({ ring }));
      expect(output, JSON.stringify(ring)).toMatch(/--ring did not answer with frames, width, height/);
      expect(status, JSON.stringify(ring)).toBe(1);
    }
  });

  it('fails a run that measured fewer or more detectors than the binary has', () => {
    for (const solved of [0, 2, 4]) {
      const { status, output } = run(buildDir({ solved }));
      expect(output, String(solved)).toMatch(new RegExp(`measured ${solved} detectors and the binary has 3`));
      expect(status, String(solved)).toBe(1);
    }
  });

  it('fails a measurement that exits non-zero, even with every line printed', () => {
    const { status, output } = run(buildDir({ exit: 1 }));
    expect(output).toMatch(/the measurement failed \(exit 1\)/);
    expect(status).toBe(1);
  });

  it('fails the whole step when one of several builds fails', () => {
    // Both orders, so a loop that stops early or keeps only the last verdict is caught either way.
    for (const dirs of [[buildDir({ solved: 2 }), buildDir()], [buildDir(), buildDir({ solved: 2 })]]) {
      expect(run(...dirs).status).toBe(1);
    }
  });
});
