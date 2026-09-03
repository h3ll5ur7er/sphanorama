/**
 * The core as the page sees it: a `CoreRuntime` whose calls cross to a worker (ADR 0019).
 *
 * The client cannot tell this from a module loaded in the page, which is the point — the typed
 * proxies are built on it by the same `coreFrom` either way, and the generated facade already
 * returned promises, so nothing above this changed shape when the core moved.
 *
 * Measured cost of the crossing: ~71 µs against ~0.4 µs for a direct call, which is 0.4% of a
 * frame at capture rate. What it buys is a 30 ms spill that no longer lands on the frame the user
 * is watching.
 */
import { coreFrom, type CoreRuntime, type HostState, type RuntimeCapabilities, type SphanoramaCore }
  from './core';
import type { CameraOpening, FromWorker, ToWorker } from './protocol';

/**
 * A request minus the number that pairs it with its answer, so callers describe what they want
 * and the sequencing stays in one place. Distributive on purpose: a plain `Omit` over a union
 * collapses it to the fields every member shares, which is only `kind`.
 */
type Unsequenced<T> = T extends { seq: number } ? Omit<T, 'seq'> : never;

/** The bits of `Worker` this uses, so a test can stand in for one without a browser. */
export interface WorkerLike {
  postMessage(message: ToWorker, transfer?: Transferable[]): void;
  addEventListener(type: 'message', listener: (event: MessageEvent<FromWorker>) => void): void;
  terminate?(): void;
}

/** What the page pushes across, and the one thing the worker asks of it. */
export interface RemoteCore extends CoreRuntime {
  setCamera(opened: CameraOpening | null): void;
  setMotion(capability: string): void;
  pushMotion(doubles: Float64Array): void;
  flush(): Promise<string | null>;
  /** Fires when `ICameraAccess::Close` reached the host: only the page can stop a MediaStream. */
  onCloseCamera(handler: () => void): void;
  /**
   * Whether the worker got a spill tier. False means the frame store has nowhere to put a frame
   * it cannot hold, so a capture is capped at what fits in RAM — degraded, not broken, and worth
   * saying out loud rather than surfacing later as a refusal nobody can explain (ADR 0020).
   */
  canSpill(): boolean;
}

export interface RemoteCoreHandle {
  core: SphanoramaCore;
  remote: RemoteCore;
}

export async function connectCore(worker: WorkerLike, coreUrl: string): Promise<RemoteCoreHandle> {
  const pending = new Map<number, { resolve: (m: FromWorker) => void; reject: (e: Error) => void }>();
  let closeCamera: () => void = () => {};
  let seq = 0;

  worker.addEventListener('message', (event: MessageEvent<FromWorker>) => {
    const message = event.data;
    if (message.kind === 'closeCamera') {
      closeCamera();
      return;
    }
    const waiting = pending.get(message.seq);
    // A reply to a request nobody is waiting for is not worth throwing over: it means the page
    // moved on, and there is nothing left to tell.
    if (!waiting) return;
    pending.delete(message.seq);
    if (message.kind === 'failed') waiting.reject(new Error(message.detail));
    else waiting.resolve(message);
  });

  const ask = (message: Unsequenced<ToWorker>, transfer?: Transferable[]) =>
    new Promise<FromWorker>((resolve, reject) => {
      const id = ++seq;
      pending.set(id, { resolve, reject });
      worker.postMessage({ ...message, seq: id } as ToWorker, transfer);
    });

  const booted = await ask({ kind: 'boot', coreUrl });
  if (booted.kind !== 'booted') throw new Error('the worker answered boot with something else');
  const methodNames = booted.methods;

  const runtime: CoreRuntime = {
    methods: () => methodNames,

    async capabilities(host: HostState): Promise<RuntimeCapabilities> {
      const answer = await ask({
        kind: 'capabilities',
        hardwareConcurrency: host.hardwareConcurrency,
        crossOriginIsolated: host.crossOriginIsolated,
      });
      if (answer.kind !== 'capabilities') throw new Error('the worker answered with something else');
      return answer.value;
    },

    async call(method: string, args: Uint8Array): Promise<Uint8Array> {
      // The method name is resolved to an id on the far side, where the table is. Sending the
      // name keeps this side from caching ids that shift the day a method is inserted above them.
      const answer = await ask({ kind: 'call', method, args });
      if (answer.kind !== 'result') throw new Error('the worker answered a call with something else');
      return answer.bytes;
    },
  };

  const remote: RemoteCore = {
    ...runtime,
    setCamera: (opened) => worker.postMessage({ kind: 'camera', opened }),
    setMotion: (capability) => worker.postMessage({ kind: 'motion', capability }),
    // Transferred, so a batch costs the same whatever its length. The buffer is not read again
    // on this side, which is what makes handing over ownership safe rather than clever.
    pushMotion: (doubles) => worker.postMessage({ kind: 'imu', doubles }, [doubles.buffer]),
    async flush(): Promise<string | null> {
      const answer = await ask({ kind: 'flush' });
      return answer.kind === 'flushed' ? answer.persistError : null;
    },
    onCloseCamera(handler: () => void) {
      closeCamera = handler;
    },
    canSpill: () => booted.spill,
  };

  return { core: coreFrom(runtime), remote };
}
