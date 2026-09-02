// Loads the shipped WASM module and checks the boundary answers. Run against the real artifact
// rather than a node-flavoured build: `-sENVIRONMENT=web,worker` is what ships, and a module
// that only works with node's loader in it would be a different binary.
//
// Usage: node bridge/test/module_smoke.mjs <path-to-sphanorama-core.js>
import { readFile } from 'node:fs/promises';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const modulePath = resolve(process.argv[2]);
const wasmPath = modulePath.replace(/\.js$/, '.wasm');

const createSphanoramaCore = (await import(pathToFileURL(modulePath))).default;

// The shipped module fetches its .wasm, which node cannot do for a file:// URL. instantiateWasm
// hands it the bytes directly, so the artifact under test is the one that ships rather than a
// node-flavoured rebuild of it.
const bytes = await readFile(wasmPath);
const core = await createSphanoramaCore({
  instantiateWasm(imports, done) {
    WebAssembly.instantiate(bytes, imports).then((result) => done(result.instance));
    return {};
  },
});

let failures = 0;
function check(label, actual, expected) {
  const ok = JSON.stringify(actual) === JSON.stringify(expected);
  if (!ok) {
    console.error(`FAIL ${label}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
    failures++;
  } else {
    console.log(`ok   ${label}`);
  }
}

const threadedBuild = process.env.SPHANORAMA_THREADED === '1';

// Without cross-origin isolation there is no SharedArrayBuffer, whatever the build supports.
// This is the GitHub Pages case and the one most likely to be got wrong optimistically.
const unisolated = core.probeRuntime(8, false);
check('no threads without cross-origin isolation', unisolated.threads, false);
check('no shared memory without cross-origin isolation', unisolated.sharedMemory, false);
check('concurrency reported as zero when unthreaded', unisolated.hardwareConcurrency, 0);

const isolated = core.probeRuntime(8, true);
check('threads follow the build', isolated.threads, threadedBuild);
check('concurrency follows the build', isolated.hardwareConcurrency, threadedBuild ? 8 : 0);

// SIMD is a build flag, and every preset sets -msimd128; losing it would silently halve
// throughput on the blend path with nothing failing.
check('simd is compiled in', isolated.simd, true);

check('a single core never reports threads', core.probeRuntime(1, true).threads, false);

console.log(failures === 0 ? '\nmodule smoke: PASS' : `\nmodule smoke: ${failures} FAILED`);
process.exit(failures === 0 ? 0 : 1);
