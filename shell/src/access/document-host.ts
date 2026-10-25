/**
 * The page half of the project-store port.
 *
 * The core's IProjectStoreAccess contract is synchronous; IndexedDB is not. Rather than make the
 * contract async — which would push V12's volatility upward into every manager that touches
 * storage — the host keeps every document resident in memory and persists behind it (ADR 0014).
 *
 * So: reads and writes are immediate and never block the core, and durability follows shortly
 * after. A write that fails to persist still stands in memory, because losing durability is bad
 * and losing the session in progress is worse.
 *
 * A store that would not load is never written to. What it holds may still be there, and a host
 * that starts empty cannot see it: project ids restart at 1 against an empty listing, so writing
 * behind would put a new capture's documents over the ones it could not read (ADR 0078).
 */

/** The persistence side, kept separate so the resident model can be tested without a database. */
export interface DocumentStore {
  loadAll(): Promise<Record<string, string>>;
  put(key: string, value: string): Promise<void>;
  remove(prefix: string): Promise<void>;
  /**
   * Hears the store lose its database after loading it, as a clear of the site's data does. A
   * persist would find out too, but none may come for as long as nothing is written (ADR 0079).
   */
  onLost?(listener: (reason: string) => void): void;
}

export interface DocumentHost {
  projectIds(): number[];
  read(project: number, key: string): string | undefined;
  write(project: number, key: string, value: string): void;
  remove(project: number): boolean;
  /** Awaits everything pending. The client calls this when the page is hidden or unloaded. */
  flush(): Promise<void>;
  /** Why the last persist failed, if it did. Durability problems are reportable, not silent. */
  lastPersistError(): string | null;
  /**
   * Why the store would not load, if it would not: the host then holds nothing it was given and
   * persists nothing, so an empty listing says nothing about what is on disk (ADR 0078).
   */
  loadError(): string | null;
  /**
   * Why persisting is failing now, or null when the last persist went through. Unlike
   * `lastPersistError()` it clears on recovery: it answers "is this being kept", which the page
   * says for the life of the page (ADR 0079). Null after a failed load, which persists nothing.
   */
  persistFailing(): string | null;
  /** Called with `persistFailing()` each time it changes. */
  onPersistChange(listener: (failing: string | null) => void): void;
}

const DEFAULT_FLUSH_DELAY_MS = 250;

function documentKey(project: number, key: string): string {
  return `${project}/${key}`;
}

export async function createDocumentHost(
  store: DocumentStore,
  options: { flushDelayMs?: number } = {},
): Promise<DocumentHost> {
  const resident = new Map<string, string>();
  const pending = new Map<string, string>();
  const removed = new Set<number>();
  let persistError: string | null = null;
  let loadError: string | null = null;
  let failing: string | null = null;
  const listeners: Array<(failing: string | null) => void> = [];
  function setFailing(now: string | null): void {
    if (now === failing) return;
    failing = now;
    // A listener is the page's business and durability is this host's: one that throws is
    // reported and does not unwind the persist that told it.
    for (const listener of listeners) {
      try {
        listener(now);
      } catch (cause) {
        console.warn('sphanorama: a persist listener threw —', String(cause));
      }
    }
  }
  let timer: ReturnType<typeof setTimeout> | null = null;
  let inFlight: Promise<void> | null = null;

  try {
    for (const [key, value] of Object.entries(await store.loadAll())) resident.set(key, value);
  } catch (cause) {
    // Private browsing, blocked storage, a corrupt database: the app runs, it just does not
    // remember anything. Refusing to start would be a worse trade.
    persistError = String(cause);
    loadError = String(cause);
  }
  if (loadError === null) {
    store.onLost?.((reason) => {
      persistError = reason;
      setFailing(reason);
    });
  }

  async function persist(): Promise<void> {
    const writes = [...pending];
    const removals = [...removed];
    pending.clear();
    removed.clear();
    try {
      for (const project of removals) await store.remove(`${project}/`);
      for (const [key, value] of writes) await store.put(key, value);
      setFailing(null);
    } catch (cause) {
      persistError = String(cause);
      // Put the work back, or a single transient quota error means the resident state can never
      // become durable again — the caller has no way to know which documents to rewrite. A newer
      // value already queued for the same key wins: this is a retry of what failed, not a
      // rollback of what happened since. So does a removal since: a document no longer resident
      // was removed after it was written, and putting it back wrote a deleted project's title
      // over the removal on the next persist.
      for (const project of removals) removed.add(project);
      for (const [key, value] of writes) {
        if (!pending.has(key) && resident.has(key)) pending.set(key, value);
      }
      setFailing(persistError);
    }
  }

  /**
   * One persist at a time, always. Overlapping snapshots can finish out of order, and a slow
   * older one landing after a newer one restores stale data; a `flush()` that awaited only the
   * handle it happened to see would also return before the other had committed.
   */
  function enqueue(): Promise<void> {
    inFlight = (inFlight ?? Promise.resolve()).then(persist, persist);
    return inFlight;
  }

  function schedule(): void {
    if (timer !== null) return;
    timer = setTimeout(() => {
      timer = null;
      void enqueue();
    }, options.flushDelayMs ?? DEFAULT_FLUSH_DELAY_MS);
  }

  return {
    projectIds(): number[] {
      const ids = new Set<number>();
      for (const key of resident.keys()) {
        const project = Number(key.slice(0, key.indexOf('/')));
        if (Number.isFinite(project)) ids.add(project);
      }
      return [...ids];
    },

    read(project: number, key: string): string | undefined {
      return resident.get(documentKey(project, key));
    },

    write(project: number, key: string, value: string): void {
      // Called from inside a synchronous C++ call. Nothing here may await, or the core would be
      // blocked on storage in the middle of a capture.
      const full = documentKey(project, key);
      resident.set(full, value);
      if (loadError !== null) return;
      pending.set(full, value);   // a Map, so repeated writes coalesce to the last value
      schedule();
    },

    remove(project: number): boolean {
      const prefix = `${project}/`;
      let found = false;
      for (const key of [...resident.keys()]) {
        if (key.startsWith(prefix)) {
          resident.delete(key);
          pending.delete(key);
          found = true;
        }
      }
      if (found && loadError === null) {
        removed.add(project);
        schedule();
      }
      return found;
    },

    async flush(): Promise<void> {
      if (timer !== null) {
        clearTimeout(timer);
        timer = null;
      }
      // Drains the queue rather than awaiting one handle: work can be added while an earlier
      // persist runs, and a flush that returned before it committed would be a durability
      // guarantee that is not one.
      await inFlight;
      while (pending.size > 0 || removed.size > 0) {
        await enqueue();
        // A failed persist requeues what it could not write. Retrying it forever here would
        // hang the page on a full disk; the caller reads persistFailing() and decides. Work left
        // after one that went through arrived while it ran, and is drained: the queue's size
        // cannot tell the two apart, and a flush that stopped on it was answered "durable".
        if (failing !== null) break;
      }
    },

    lastPersistError(): string | null {
      return persistError;
    },

    loadError(): string | null {
      return loadError;
    },

    persistFailing(): string | null {
      return failing;
    },

    onPersistChange(listener: (failing: string | null) => void): void {
      listeners.push(listener);
    },
  };
}
