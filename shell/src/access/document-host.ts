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
 */

/** The persistence side, kept separate so the resident model can be tested without a database. */
export interface DocumentStore {
  loadAll(): Promise<Record<string, string>>;
  put(key: string, value: string): Promise<void>;
  remove(prefix: string): Promise<void>;
}

export interface DocumentHost {
  projectIds(): number[];
  read(project: number, key: string): string | undefined;
  write(project: number, key: string, value: string): void;
  remove(project: number): boolean;
  /** Awaits everything pending. The client calls this at session end and before unload. */
  flush(): Promise<void>;
  /** Why the last persist failed, if it did. Durability problems are reportable, not silent. */
  lastPersistError(): string | null;
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
  let timer: ReturnType<typeof setTimeout> | null = null;
  let inFlight: Promise<void> | null = null;

  try {
    for (const [key, value] of Object.entries(await store.loadAll())) resident.set(key, value);
  } catch (cause) {
    // Private browsing, blocked storage, a corrupt database: the app runs, it just does not
    // remember anything. Refusing to start would be a worse trade.
    persistError = String(cause);
  }

  async function persist(): Promise<void> {
    const writes = [...pending];
    const removals = [...removed];
    pending.clear();
    removed.clear();
    try {
      for (const project of removals) await store.remove(`${project}/`);
      for (const [key, value] of writes) await store.put(key, value);
    } catch (cause) {
      persistError = String(cause);
      // Put the work back, or a single transient quota error means the resident state can never
      // become durable again — the caller has no way to know which documents to rewrite. A newer
      // value already queued for the same key wins: this is a retry of what failed, not a
      // rollback of what happened since.
      for (const project of removals) removed.add(project);
      for (const [key, value] of writes) if (!pending.has(key)) pending.set(key, value);
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
      if (found) {
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
        const before = pending.size + removed.size;
        await enqueue();
        // A failed persist requeues what it could not write. Retrying it forever here would
        // hang the page on a full disk; the caller reads lastPersistError() and decides.
        if (pending.size + removed.size >= before) break;
      }
    },

    lastPersistError(): string | null {
      return persistError;
    },
  };
}
