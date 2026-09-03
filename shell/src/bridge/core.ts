/**
 * Typed loader for the WASM core.
 *
 * The boundary is a C ABI over the shared heap (ADR 0012), and the module publishes both its
 * probe layout and its method table, so nothing here re-declares either. That is why this file
 * can be short: it marshals and wires, it decides nothing.
 */
import { createFacadeCall, type FacadeCall, type FacadeModule } from './facade';
import {
  createCaptureSessionManagerProxy,
  createPanoramaBuildManagerProxy,
  createProjectManagerProxy,
} from './facade.generated';

/** The subset of the Emscripten module this loader touches. */
export interface EmscriptenModule extends FacadeModule {
  HEAP32: Int32Array;
  UTF8ToString(pointer: number): string;
  _sph_probe_field_count(): number;
  _sph_probe_field_name(index: number): number;
  _sph_probe_runtime(concurrency: number, crossOriginIsolated: number, out: number): number;
  _malloc(bytes: number): number;
  _free(pointer: number): void;
}

export interface RuntimeCapabilities {
  simd: boolean;
  threads: boolean;
  sharedMemory: boolean;
  hardwareConcurrency: number;
}

/**
 * State only the host knows. The core answers from the build it is in and cannot see whether the
 * page was served cross-origin isolated, so the caller supplies it (ADR 0011).
 */
export interface HostState {
  hardwareConcurrency: number;
  crossOriginIsolated: boolean;
}

/**
 * What a core offers before the typed proxies are put on it.
 *
 * Two things provide this and the client cannot tell them apart: the module itself, in the
 * context it was loaded in, and a stand-in that forwards to the worker holding one (ADR 0019).
 * Splitting it here is what stops the proxies being built twice, once per side.
 */
export interface CoreRuntime {
  methods(): string[];
  capabilities(host: HostState): Promise<RuntimeCapabilities>;
  call: FacadeCall;
}

export interface SphanoramaCore extends CoreRuntime {
  project: ReturnType<typeof createProjectManagerProxy>;
  captureSession: ReturnType<typeof createCaptureSessionManagerProxy>;
  panoramaBuild: ReturnType<typeof createPanoramaBuildManagerProxy>;
}

/** Puts the generated proxies on a runtime, wherever the module behind it happens to be. */
export function coreFrom(runtime: CoreRuntime): SphanoramaCore {
  return {
    ...runtime,
    project: createProjectManagerProxy(runtime.call),
    captureSession: createCaptureSessionManagerProxy(runtime.call),
    panoramaBuild: createPanoramaBuildManagerProxy(runtime.call),
  };
}

export type ModuleFactory = () => Promise<EmscriptenModule>;

/**
 * The core in this context: the module is here and every call is a direct one.
 *
 * `capabilities` is a promise even though nothing here awaits, because the interface it satisfies
 * has to hold for a core on the other side of a `postMessage` too.
 */
export async function loadCoreRuntime(factory: ModuleFactory): Promise<CoreRuntime> {
  const module = await factory();

  const fieldCount = module._sph_probe_field_count();
  if (fieldCount <= 0) {
    throw new Error(
      'core published no probe fields — the export table was probably stripped at link time',
    );
  }
  const fields = Array.from({ length: fieldCount }, (_unused, index) =>
    module.UTF8ToString(module._sph_probe_field_name(index)));

  // One call path, shared by every proxy: names resolved once here, ids never seen by a client.
  const call = createFacadeCall(module);
  const methodNames = Array.from(
    { length: module._sph_facade_method_count() },
    (_unused, id) => module.UTF8ToString(module._sph_facade_method_name(id)));

  return {
    methods: () => methodNames,
    call,

    async capabilities(host: HostState): Promise<RuntimeCapabilities> {
      const pointer = module._malloc(fieldCount * 4);
      // Checked before the try, exactly as the facade call path does. Zero means the allocation
      // failed, and it is also a valid heap offset — so using it writes the capability fields
      // over the start of linear memory, and the _free(0) in the finally then swallows the
      // original failure. This is the second of the two malloc sites in this bundle; fixing one
      // and not the other is how a class of bug survives being fixed.
      if (pointer === 0) {
        throw new Error(`core could not allocate ${fieldCount * 4} bytes for its probe`);
      }
      try {
        const status = module._sph_probe_runtime(
          host.hardwareConcurrency,
          host.crossOriginIsolated ? 1 : 0,
          pointer,
        );
        if (status !== 0) throw new Error(`core probe failed with status ${status}`);

        const base = pointer >> 2;
        const raw: Record<string, number> = {};
        fields.forEach((name, index) => {
          raw[name] = module.HEAP32[base + index];
        });

        return {
          simd: raw.simd === 1,
          threads: raw.threads === 1,
          sharedMemory: raw.sharedMemory === 1,
          hardwareConcurrency: raw.hardwareConcurrency ?? 0,
        };
      } finally {
        // Runs on the failure path too: the probe is called at startup and whenever capabilities
        // change, so a leak here would be unbounded.
        module._free(pointer);
      }
    },
  };
}

/** The whole thing, for a module in this context. */
export async function loadCore(factory: ModuleFactory): Promise<SphanoramaCore> {
  return coreFrom(await loadCoreRuntime(factory));
}
