// The loader decodes the C ABI. Its tests use a fake Emscripten module rather than the real WASM
// build: what is being tested is the decoding contract, and a test that needs a 7 KB binary and a
// browser to check an off-by-one in a field index is a test nobody runs.
import { describe, expect, it, vi } from 'vitest';

import { loadCore } from './core';
import type { EmscriptenModule } from './core';

/** A stand-in for the compiled module, laid out the way the real one describes itself. */
function fakeModule(options: {
  fields?: string[];
  values?: Record<string, number>;
  probeStatus?: number;
} = {}): EmscriptenModule & { freed: number[]; allocated: number[] } {
  const fields = options.fields ?? ['simd', 'threads', 'sharedMemory', 'hardwareConcurrency'];
  const values = options.values ?? { simd: 1, threads: 0, sharedMemory: 0, hardwareConcurrency: 0 };
  const heap = new Int32Array(256);
  const allocated: number[] = [];
  const freed: number[] = [];
  let next = 64;

  return {
    HEAP32: heap,
    UTF8ToString: (pointer: number) => fields[pointer - 1000] ?? '',
    _sph_probe_field_count: () => fields.length,
    _sph_probe_field_name: (index: number) => 1000 + index,
    _sph_probe_runtime: (_concurrency: number, _isolated: number, out: number) => {
      if (options.probeStatus) return options.probeStatus;
      fields.forEach((name, i) => {
        heap[(out >> 2) + i] = values[name] ?? 0;
      });
      return 0;
    },
    _malloc: (bytes: number) => {
      const pointer = next;
      next += bytes;
      allocated.push(pointer);
      return pointer;
    },
    _free: (pointer: number) => {
      freed.push(pointer);
    },
    allocated,
    freed,
  };
}

describe('loadCore', () => {
  it('reports the capabilities the module writes', async () => {
    const core = await loadCore(async () => fakeModule({
      values: { simd: 1, threads: 1, sharedMemory: 1, hardwareConcurrency: 8 },
    }));
    expect(core.capabilities({ hardwareConcurrency: 8, crossOriginIsolated: true })).toEqual({
      simd: true,
      threads: true,
      sharedMemory: true,
      hardwareConcurrency: 8,
    });
  });

  it('decodes by name rather than by position', async () => {
    // The module publishes its own field order (ADR 0012). A loader that assumed positions would
    // read plausible nonsense the day a field is inserted, and nothing would fail.
    const module = fakeModule({
      fields: ['hardwareConcurrency', 'sharedMemory', 'threads', 'simd'],
      values: { simd: 1, threads: 0, sharedMemory: 1, hardwareConcurrency: 4 },
    });
    const core = await loadCore(async () => module);
    expect(core.capabilities({ hardwareConcurrency: 4, crossOriginIsolated: true })).toEqual({
      simd: true,
      threads: false,
      sharedMemory: true,
      hardwareConcurrency: 4,
    });
  });

  it('passes the host state the core cannot see for itself', async () => {
    const module = fakeModule();
    const spy = vi.spyOn(module, '_sph_probe_runtime');
    const core = await loadCore(async () => module);
    core.capabilities({ hardwareConcurrency: 12, crossOriginIsolated: false });
    expect(spy).toHaveBeenCalledWith(12, 0, expect.any(Number));
  });

  it('frees its scratch buffer even when the probe fails', async () => {
    // The probe runs on every startup and on capability changes; a leak here is unbounded.
    const module = fakeModule({ probeStatus: 7 });
    const core = await loadCore(async () => module);
    expect(() => core.capabilities({ hardwareConcurrency: 1, crossOriginIsolated: false }))
      .toThrow(/status 7/);
    expect(module.freed).toEqual(module.allocated);
  });

  it('frees its scratch buffer on success too', async () => {
    const module = fakeModule();
    const core = await loadCore(async () => module);
    core.capabilities({ hardwareConcurrency: 2, crossOriginIsolated: false });
    expect(module.freed).toEqual(module.allocated);
  });

  it('rejects a module that publishes no fields', async () => {
    // An empty layout means the export table was stripped, which otherwise surfaces much later
    // as capabilities that are all silently false.
    await expect(loadCore(async () => fakeModule({ fields: [] })))
      .rejects.toThrow(/no probe fields/i);
  });

  it('surfaces a module that fails to instantiate', async () => {
    await expect(loadCore(async () => { throw new Error('wasm fetch failed'); }))
      .rejects.toThrow(/wasm fetch failed/);
  });
});
