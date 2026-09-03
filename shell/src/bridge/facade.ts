/**
 * The call path into the core.
 *
 * One entry point, one result buffer, method ids resolved from the names the core publishes —
 * so a client never hard-codes an id that shifts the day a method is inserted above it
 * (ADR 0012, ADR 0013). The typed proxies over this are generated.
 */
import { Reader } from './wire';
import type { Status, StatusCode } from '../../../contracts/ts/contracts';
import { StatusCodeValues } from './codec.generated';

/** The subset of the Emscripten module the facade touches. */
export interface FacadeModule {
  // ArrayBufferLike, not ArrayBuffer: in the threaded build the heap is backed by a
  // SharedArrayBuffer (ADR 0011), and typing it narrowly would compile for one artifact only.
  readonly HEAPU8: Uint8Array<ArrayBufferLike>;
  UTF8ToString(pointer: number): string;
  _sph_facade_method_count(): number;
  _sph_facade_method_name(id: number): number;
  _sph_facade_call(id: number, args: number, length: number): number;
  _sph_facade_result(): number;
  _malloc(bytes: number): number;
  _free(pointer: number): void;
}

export type FacadeCall = (name: string, args: Uint8Array) => Promise<Uint8Array>;

/** A response that did not decode. Shaped like any other failure so a client branches on it. */
export function malformedResponse(detail: string): Status {
  return { code: 'Internal', component: 'facade', detail };
}

export function decodeStatus(input: Reader): Status {
  const index = input.i32();
  const component = input.string();
  const detail = input.string();
  // Checked before the fields are trusted. A failed Reader returns zero for every read past the
  // end, and zero is the index of Ok — so a clipped or empty result buffer would otherwise
  // arrive as a successful call with default-valued everything.
  if (!input.ok) {
    return malformedResponse('malformed response: the status did not decode');
  }
  // An unknown index is not guessed at: reporting Internal with the original detail keeps the
  // core's explanation rather than replacing it with a wrong name.
  const code: StatusCode = StatusCodeValues[index] ?? 'Internal';
  return { code, component, detail };
}

export function createFacadeCall(module: FacadeModule): FacadeCall {
  const ids = new Map<string, number>();
  for (let id = 0; id < module._sph_facade_method_count(); id++) {
    ids.set(module.UTF8ToString(module._sph_facade_method_name(id)), id);
  }

  return async function call(name: string, args: Uint8Array): Promise<Uint8Array> {
    const id = ids.get(name);
    if (id === undefined) {
      // Naming the method here beats a silent empty result surfacing somewhere else entirely.
      throw new Error(`core has no method '${name}' — the bundle and the core disagree`);
    }

    // malloc(0) is allowed to return null, so give a zero-argument call one byte to point at.
    const pointer = module._malloc(Math.max(args.length, 1));
    // Null means the allocation failed, which with memory growth enabled is a real outcome on a
    // phone that already has a sphere of frames pinned. Zero is also a valid heap offset, so
    // using it writes the arguments over the start of the heap and calls into C++ with a null
    // pointer — corruption that surfaces as whatever fails next rather than as this.
    if (pointer === 0) {
      throw new Error(`core could not allocate ${args.length} bytes for '${name}'`);
    }
    try {
      // HEAPU8 is read fresh: Emscripten replaces the view when memory grows, and a cached one
      // would be detached.
      module.HEAPU8.set(args, pointer);
      const length = module._sph_facade_call(id, pointer, args.length);
      const at = module._sph_facade_result();
      // A copy, not a view: the core reuses this buffer on the next call, and Emscripten may
      // replace the heap under us. Constructing from the subarray also lands the result in a
      // plain ArrayBuffer regardless of which build produced it.
      return new Uint8Array(module.HEAPU8.subarray(at, at + length));
    } finally {
      module._free(pointer);
    }
  };
}
