// The facade call path, against a fake module. What is under test is the marshalling contract —
// name resolution, heap handling, failure decoding — not the core's behaviour, which has its own
// suite natively.
import { describe, expect, it, vi } from 'vitest';

import { createFacadeCall, decodeStatus, type FacadeModule } from './facade';
import { Reader, Writer } from './wire';

/** A module that echoes a scripted response and records what it was handed. */
function fakeModule(options: {
  methods?: string[];
  respond?: (id: number, args: Uint8Array<ArrayBufferLike>) => Uint8Array<ArrayBufferLike>;
  growHeapOnCall?: boolean;
} = {}) {
  const methods = options.methods ?? ['ProjectManager.create', 'ProjectManager.list'];
  let heap = new Uint8Array(4096);
  const nameOffsets = new Map<number, string>();
  let next = 1024;
  let lastResult: Uint8Array<ArrayBufferLike> = new Uint8Array(0);
  const calls: Array<{ id: number; args: Uint8Array<ArrayBufferLike> }> = [];
  const freed: number[] = [];

  const module: FacadeModule & { calls: typeof calls; freed: number[] } = {
    get HEAPU8() {
      return heap;
    },
    UTF8ToString: (pointer: number) => nameOffsets.get(pointer) ?? '',
    _sph_facade_method_count: () => methods.length,
    _sph_facade_method_name: (id: number) => {
      const pointer = 100 + id;
      nameOffsets.set(pointer, methods[id] ?? '');
      return pointer;
    },
    _sph_facade_call: (id: number, argsPointer: number, length: number) => {
      const args = heap.slice(argsPointer, argsPointer + length);
      calls.push({ id, args });
      // Emscripten swaps HEAPU8 for a new view when memory grows; a caller holding the old one
      // is reading a detached buffer.
      if (options.growHeapOnCall) heap = new Uint8Array(heap.length * 2);
      lastResult = options.respond?.(id, args) ?? new Uint8Array(0);
      heap.set(lastResult, 2048);
      return lastResult.length;
    },
    _sph_facade_result: () => 2048,
    _malloc: (bytes: number) => {
      const pointer = next;
      next += bytes;
      return pointer;
    },
    _free: (pointer: number) => {
      freed.push(pointer);
    },
    calls,
    freed,
  };
  return module;
}

function okStatus(): Uint8Array {
  const w = new Writer();
  w.i32(0);          // StatusCode.Ok
  w.string('');
  w.string('');
  return w.finish();
}

describe('createFacadeCall', () => {
  it('resolves method names to ids once, at construction', () => {
    const module = fakeModule();
    const spy = vi.spyOn(module, '_sph_facade_method_name');
    const call = createFacadeCall(module);
    const before = spy.mock.calls.length;
    void call('ProjectManager.list', new Uint8Array(0));
    expect(spy.mock.calls.length).toBe(before);
  });

  it('calls the id the core published for that name', async () => {
    const module = fakeModule({ respond: () => okStatus() });
    const call = createFacadeCall(module);
    await call('ProjectManager.list', new Uint8Array(0));
    expect(module.calls[0]?.id).toBe(1);
  });

  it('passes the encoded arguments through unchanged', async () => {
    const module = fakeModule({ respond: () => okStatus() });
    const call = createFacadeCall(module);
    await call('ProjectManager.create', new Uint8Array([1, 2, 3, 4]));
    expect(Array.from(module.calls[0]!.args)).toEqual([1, 2, 3, 4]);
  });

  it('rejects a method the core does not have', async () => {
    // A client bundle can be older or newer than the core it loaded. Failing at the call site
    // names the method; a silent no-op would surface as an empty result much later.
    const call = createFacadeCall(fakeModule());
    await expect(call('ProjectManager.nope', new Uint8Array(0)))
      .rejects.toThrow(/ProjectManager\.nope/);
  });

  it('frees the argument buffer it allocated', async () => {
    const module = fakeModule({ respond: () => okStatus() });
    const call = createFacadeCall(module);
    await call('ProjectManager.create', new Uint8Array([1, 2, 3]));
    expect(module.freed.length).toBe(1);
  });

  it('frees the argument buffer even when the call fails', async () => {
    const module = fakeModule({
      respond: () => { throw new Error('core trapped'); },
    });
    const call = createFacadeCall(module);
    await expect(call('ProjectManager.create', new Uint8Array([1]))).rejects.toThrow();
    expect(module.freed.length).toBe(1);
  });

  it('copies the result out of the heap rather than viewing into it', async () => {
    // The core reuses one result buffer, and Emscripten replaces HEAPU8 wholesale when memory
    // grows. A view into either would read someone else's bytes on the next call.
    const module = fakeModule({ growHeapOnCall: true, respond: () => okStatus() });
    const call = createFacadeCall(module);
    const first = await call('ProjectManager.list', new Uint8Array(0));
    await call('ProjectManager.list', new Uint8Array(0));
    expect(first.length).toBe(okStatus().length);
    expect(Array.from(first)).toEqual(Array.from(okStatus()));
  });

  it('handles an empty argument list without allocating nothing', async () => {
    const module = fakeModule({ respond: () => okStatus() });
    const call = createFacadeCall(module);
    const result = await call('ProjectManager.list', new Uint8Array(0));
    expect(result.length).toBeGreaterThan(0);
  });
});

describe('decodeStatus', () => {
  it('reads a success status', () => {
    const status = decodeStatus(new Reader(okStatus()));
    expect(status.code).toBe('Ok');
  });

  it('reads a failure with its component and detail', () => {
    const w = new Writer();
    w.i32(1);                        // InvalidArgument
    w.string('ProjectManager');
    w.string('a project needs a title');
    const status = decodeStatus(new Reader(w.finish()));
    expect(status.code).toBe('InvalidArgument');
    expect(status.component).toBe('ProjectManager');
    expect(status.detail).toBe('a project needs a title');
  });

  it('does not invent a code for an index it does not know', () => {
    // A core newer than this bundle can return a status code this build has never heard of.
    const w = new Writer();
    w.i32(9999);
    w.string('X');
    w.string('from the future');
    const status = decodeStatus(new Reader(w.finish()));
    expect(status.code).toBe('Internal');
    expect(status.detail).toContain('from the future');
  });
});
