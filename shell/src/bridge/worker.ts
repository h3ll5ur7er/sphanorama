/**
 * The context the core runs in (ADR 0019).
 *
 * It holds the WASM module, the document host — IndexedDB works here, so that port came across
 * unchanged and ADR 0014 applies to it exactly as written — and the resident halves of the camera
 * and motion ports, which the page keeps fed because `getUserMedia` and `deviceorientation` are
 * page things.
 *
 * Nothing here decides anything. It is the composition root for this side of the boundary, and
 * the reason it exists is that `createSyncAccessHandle` is worker-only: a frame store that has to
 * fault a spilled frame back in synchronously cannot do it from the main thread.
 */
import { createCaptureHost } from '../access/capture-host';
import { createDocumentHost, type DocumentHost } from '../access/document-host';
import { createIndexedDbStore } from '../access/indexeddb-store';
import { loadCoreRuntime, type CoreRuntime } from './core';
import type { FromWorker, ToWorker } from './protocol';

const scope = self as unknown as {
  postMessage(message: FromWorker, transfer?: Transferable[]): void;
  onmessage: ((event: MessageEvent<ToWorker>) => void) | null;
};

const captureHost = createCaptureHost({
  // The stream lives on the page; all this side can do is ask. Posted rather than awaited,
  // because the C++ call that reached here is synchronous and has nowhere to wait (ADR 0019).
  onCloseCamera: () => scope.postMessage({ kind: 'closeCamera' }),
});

let runtime: CoreRuntime | null = null;
let documents: DocumentHost | null = null;

function fail(seq: number, cause: unknown): void {
  scope.postMessage({ kind: 'failed', seq, detail: String(cause) });
}

async function boot(seq: number, coreUrl: string): Promise<void> {
  // Hydrated before the module, because the core reads documents through a synchronous port and
  // a store that is still loading would answer "no such project" to a session it should resume.
  documents = await createDocumentHost(createIndexedDbStore());

  // Imported at runtime rather than bundled: the module is an artifact of the C++ build, and the
  // two builds (ADR 0011) are selected by which one the deploy copied in. The page resolved the
  // URL, because it is the side that knows the base path.
  const factory = (await import(/* @vite-ignore */ coreUrl)).default;
  const host = { ...documents, ...captureHost };
  runtime = await loadCoreRuntime(async () => factory({ sphHost: host }));

  scope.postMessage({ kind: 'booted', seq, methods: runtime.methods() });
}

scope.onmessage = (event: MessageEvent<ToWorker>) => {
  const message = event.data;
  void (async () => {
    try {
      switch (message.kind) {
        case 'boot':
          await boot(message.seq, message.coreUrl);
          return;

        case 'call': {
          if (!runtime) throw new Error('the core is not loaded');
          const bytes = await runtime.call(message.method, message.args);
          // Transferred: the buffer was made by the call path and nothing here reads it again.
          scope.postMessage({ kind: 'result', seq: message.seq, bytes }, [bytes.buffer]);
          return;
        }

        case 'capabilities': {
          if (!runtime) throw new Error('the core is not loaded');
          const value = await runtime.capabilities({
            hardwareConcurrency: message.hardwareConcurrency,
            crossOriginIsolated: message.crossOriginIsolated,
          });
          scope.postMessage({ kind: 'capabilities', seq: message.seq, value });
          return;
        }

        case 'flush': {
          if (documents) await documents.flush();
          scope.postMessage({
            kind: 'flushed',
            seq: message.seq,
            persistError: documents?.lastPersistError() ?? null,
          });
          return;
        }

        // The pushes. No reply, and none of them can fail in a way the page could act on: the
        // core reads whatever is here the next time it looks.
        case 'camera':
          if (message.opened) captureHost.setCamera(message.opened);
          else captureHost.clearCamera();
          return;

        case 'motion':
          captureHost.setMotion(message.capability);
          return;

        case 'imu':
          captureHost.pushMotion(message.doubles);
          return;
      }
    } catch (cause) {
      // Only requests have somewhere to report to. A push that threw would be a bug here rather
      // than something the page asked for, and swallowing it silently is worse than logging it.
      if ('seq' in message) fail(message.seq, cause);
      else console.error('sphanorama worker: unhandled failure', cause);
    }
  })();
};
