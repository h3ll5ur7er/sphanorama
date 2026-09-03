/**
 * Typed loader for the WASM core.
 *
 * The boundary is a C ABI over the shared heap (ADR 0012), and the module publishes both its
 * probe layout and its method table, so nothing here re-declares either. That is why this file
 * can be short: it marshals and wires, it decides nothing.
 */
import { createFacadeCall, type FacadeModule } from './facade';
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

export interface SphanoramaCore {
  capabilities(host: HostState): RuntimeCapabilities;

  /** Method names the core published, in id order. Useful for diagnostics and version checks. */
  methods(): string[];

  project: ReturnType<typeof createProjectManagerProxy>;
  captureSession: ReturnType<typeof createCaptureSessionManagerProxy>;
  panoramaBuild: ReturnType<typeof createPanoramaBuildManagerProxy>;
}

export type ModuleFactory = () => Promise<EmscriptenModule>;

export async function loadCore(factory: ModuleFactory): Promise<SphanoramaCore> {
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
    project: createProjectManagerProxy(call),
    captureSession: createCaptureSessionManagerProxy(call),
    panoramaBuild: createPanoramaBuildManagerProxy(call),

    capabilities(host: HostState): RuntimeCapabilities {
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
