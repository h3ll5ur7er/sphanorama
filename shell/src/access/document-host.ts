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
 * that starts empty cannot see it (ADR 0078). Project ids no longer restart at 1 against an empty
 * listing (ADR 0081), so a new capture's documents would land beside those rather than over them;
 * the guard is kept rather than revisited, since this host knows nothing of what that store holds.
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
  /**
   * An id for a new project, or null when there is none a double holds exactly. Creates nothing.
   *
   * Taken from the clock rather than counted from what is resident, because what is resident is
   * what this page loaded: another tab may have written one past the highest id in it since
   * (ADR 0081). Seconds since the epoch over twenty random bits, so tabs issuing in the same
   * second are apart by the draw, and always above every id this host has held, drawn in the next
   * second above it where the clock is behind, so a page's ids ascend however its clock moves.
   */
  issueProjectId(): number | null;
  /** Awaits everything pending. The client calls this when the page is hidden or unloaded. */
  flush(): Promise<void>;
  /**
   * Why the store would not load, if it would not: the host then holds nothing it was given and
   * persists nothing, so an empty listing says nothing about what is on disk (ADR 0078).
   */
  loadError(): string | null;
  /**
   * Why persisting is failing now, or null when the last persist went through. It clears on
   * recovery: it answers "is this being kept", which the page says for the life of the page
   * (ADR 0079). A store that lost its database never recovers, so
   * that loss is kept whatever persists after it. Null after a failed load, which persists nothing.
   */
  persistFailing(): string | null;
  /** Called with `persistFailing()` each time it changes. */
  onPersistChange(listener: (failing: string | null) => void): void;
}

const DEFAULT_FLUSH_DELAY_MS = 250;

const DRAWN_BITS = 20;

function drawLowBits(): number {
  const bits = new Uint32Array(1);
  crypto.getRandomValues(bits);
  return bits[0] % 2 ** DRAWN_BITS;
}

/**
 * The project a document key names, or null for a key no id could have written. Past what a double
 * holds exactly too, since such a key is listed and an issued id must be above it.
 */
function projectOf(key: string): number | null {
  const project = Number(key.slice(0, key.indexOf('/')));
  return Number.isInteger(project) && project > 0 ? project : null;
}

function documentKey(project: number, key: string): string {
  return `${project}/${key}`;
}

export async function createDocumentHost(
  store: DocumentStore,
  options: {
    flushDelayMs?: number;
    /** Milliseconds since the epoch. */
    now?: () => number;
    /** A whole number under 2^20. */
    draw?: () => number;
  } = {},
): Promise<DocumentHost> {
  const now = options.now ?? Date.now;
  const draw = options.draw ?? drawLowBits;
  const resident = new Map<string, string>();
  const pending = new Map<string, string>();
  const removed = new Set<number>();
  let loadError: string | null = null;
  // The highest id issued, loaded or written, removed or not: what an issued id must be above.
  let highest = 0;
  let failing: string | null = null;
  // Kept for good once heard: the database the documents were in is gone, so a persist that goes
  // through after it, even one with nothing in it, says nothing about them (ADR 0079).
  let lost: string | null = null;
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
  let inFlight: Promise<boolean> | null = null;

  try {
    for (const [key, value] of Object.entries(await store.loadAll())) {
      resident.set(key, value);
      highest = Math.max(highest, projectOf(key) ?? 0);
    }
  } catch (cause) {
    // Private browsing, blocked storage, a corrupt database: the app runs, it just does not
    // remember anything. Refusing to start would be a worse trade.
    loadError = String(cause);
  }
  if (loadError === null) {
    store.onLost?.((reason) => {
      lost = reason;
      setFailing(reason);
    });
  }

  /** Whether it went through, which after a loss is not what `failing` says. */
  async function persist(): Promise<boolean> {
    const writes = [...pending];
    const removals = [...removed];
    pending.clear();
    removed.clear();
    try {
      for (const project of removals) await store.remove(`${project}/`);
      for (const [key, value] of writes) await store.put(key, value);
      setFailing(lost);
      return true;
    } catch (cause) {
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
      // A loss stays the reason: what follows from it says less about why than it does.
      setFailing(lost ?? String(cause));
      return false;
    }
  }

  /**
   * One persist at a time, always. Overlapping snapshots can finish out of order, and a slow
   * older one landing after a newer one restores stale data; a `flush()` that awaited only the
   * handle it happened to see would also return before the other had committed.
   */
  function enqueue(): Promise<boolean> {
    inFlight = (inFlight ?? Promise.resolve(true)).then(persist, persist);
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
      highest = Math.max(highest, projectOf(full) ?? 0);
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

    issueProjectId(): number | null {
      const drawn = draw();
      const timed = Math.floor(now() / 1000) * 2 ** DRAWN_BITS + drawn;
      // Where an id held is ahead of the clock, or the clock gives no id a double holds, the next
      // second's ids above it, still drawn: every tab that loaded it holds the same one, and one
      // past it is the same id in all of them. Past the last second, one past it, undrawn.
      const above = (Math.floor(highest / 2 ** DRAWN_BITS) + 1) * 2 ** DRAWN_BITS + drawn;
      let id = Number.isSafeInteger(timed) && timed > highest ? timed : above;
      if (id > Number.MAX_SAFE_INTEGER) id = highest + 1;
      if (id > Number.MAX_SAFE_INTEGER) return null;
      highest = id;
      return id;
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
        // A failed persist requeues what it could not write. Retrying it forever here would
        // hang the page on a full disk; the caller reads persistFailing() and decides. Work left
        // after one that went through arrived while it ran, and is drained: the queue's size
        // cannot tell the two apart, and a flush that stopped on it was answered "durable".
        // Nor can `failing`, which a loss holds whether or not the persist went through.
        if (!(await enqueue())) break;
      }
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
