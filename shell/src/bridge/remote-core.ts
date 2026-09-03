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
import type { GrabbedFrame } from '../access/preview-frame';
import type { CameraOpening, FromWorker, LockReport, ToWorker } from './protocol';

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
  /**
   * The two ways a worker stops answering without saying so: `error` for a script that would not
   * load or threw at the top level, `messageerror` for a message that arrived and could not be
   * deserialised. Neither carries a sequence number, so neither can be paired with the call that
   * is waiting — every pending call has to be failed at once.
   */
  addEventListener(type: 'error' | 'messageerror', listener: (event: unknown) => void): void;
  terminate?(): void;
}

/** What the page pushes across, and the one thing the worker asks of it. */
export interface RemoteCore extends CoreRuntime {
  setCamera(opened: CameraOpening | null): void;
  setMotion(capability: string): void;
  pushMotion(doubles: Float64Array): void;
  /**
   * Hands the latest grabbed frame to the worker, by transfer (ADR 0021).
   *
   * The buffer is detached here afterwards, which is why the grabber allocates a fresh one each
   * time. Nothing waits: the core reads it from resident state on its next peek, the same trade
   * every push here takes (ADR 0014).
   */
  pushFrame(frame: GrabbedFrame): void;
  /**
   * Tells the worker which locks the camera actually settled on (ADR 0022).
   *
   * Sent before a burst is armed, because that is when the core reads it. A push like the others:
   * there is nothing to wait for, the core looks when it next needs to know.
   */
  setLocks(held: LockReport): void;
  flush(): Promise<string | null>;
  /** Fires when `ICameraAccess::Close` reached the host: only the page can stop a MediaStream. */
  onCloseCamera(handler: () => void): void;
  /** Fires when the core wants the locks back: only the page can apply constraints to a track. */
  onReleaseLocks(handler: () => void): void;
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
  let releaseLocks: () => void = () => {};
  let seq = 0;
  // Set once the worker has failed, and never cleared: a worker that could not load its script is
  // not going to load it later, and a call made after that has to fail immediately rather than
  // join a queue nothing will ever drain.
  let dead: Error | null = null;

  /**
   * Fails every call in flight and every call after it.
   *
   * Without this the promises simply stay pending. `main()` awaits the boot, so a worker whose
   * script 404s or throws at the top level left the page on its loading screen for ever — the one
   * failure mode where the user gets no message at all, because the code that writes the failure
   * UI is downstream of the await that never returns.
   */
  const die = (reason: string): void => {
    if (dead) return;
    dead = new Error(reason);
    for (const waiting of pending.values()) waiting.reject(dead);
    pending.clear();
    // Nothing is coming from it and nothing more will be sent to it. Releasing it here means a
    // page that reports the failure is not also holding a thread that failed to start.
    worker.terminate?.();
  };

  worker.addEventListener('error', (event: unknown) => {
    const message = (event as { message?: string } | null)?.message;
    die(`the core worker failed to start: ${message ?? 'no detail'}`);
  });
  worker.addEventListener('messageerror', () => {
    die('the core worker sent a message this page could not deserialise');
  });

  worker.addEventListener('message', (event: MessageEvent<FromWorker>) => {
    const message = event.data;
    if (message.kind === 'closeCamera') {
      closeCamera();
      return;
    }
    if (message.kind === 'releaseLocks') {
      releaseLocks();
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
      if (dead) {
        reject(dead);
        return;
      }
      const id = ++seq;
      pending.set(id, { resolve, reject });
      worker.postMessage({ ...message, seq: id } as ToWorker, transfer);
    });

  // Every way the handshake can fail takes the worker down with it. A boot that answers `failed`
  // — the module import threw, say, which happens *after* the OPFS handle has opened — rejects
  // the promise and leaves `connectCore` with nothing to return, so nobody holds a reference to
  // the worker any more and nobody can stop it. It keeps its sync access handle, which is
  // exclusive: the next attempt to open the same file fails, so a page that retried after a
  // failed boot would be blocked by its own orphan.
  //
  // `die` is idempotent, so an `error` event that arrives alongside this is not a second death.
  let booted: FromWorker;
  try {
    booted = await ask({ kind: 'boot', coreUrl });
  } catch (cause) {
    die(`the core worker failed to boot: ${cause instanceof Error ? cause.message : String(cause)}`);
    throw cause;
  }
  if (booted.kind !== 'booted') {
    die('the worker answered boot with something else');
    throw new Error('the worker answered boot with something else');
  }
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
    pushFrame: ({ width, height, bytes }) =>
      worker.postMessage({ kind: 'frame', width, height, bytes }, [bytes.buffer]),
    setLocks: (held) => worker.postMessage({ kind: 'locks', held }),
    async flush(): Promise<string | null> {
      const answer = await ask({ kind: 'flush' });
      return answer.kind === 'flushed' ? answer.persistError : null;
    },
    onCloseCamera(handler: () => void) {
      closeCamera = handler;
    },
    onReleaseLocks(handler: () => void) {
      releaseLocks = handler;
    },
    canSpill: () => booted.spill,
  };

  return { core: coreFrom(runtime), remote };
}
