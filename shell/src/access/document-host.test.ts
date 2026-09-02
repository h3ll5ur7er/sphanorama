// The document host is the page half of the project-store port.
//
// The core's contract is synchronous and IndexedDB is not, so the host keeps every document
// resident and persists behind it (ADR 0014). What is worth testing is exactly that seam: reads
// and writes are immediate, persistence is eventual, and a failure to persist must not corrupt
// what the core can see.
import { describe, expect, it, vi } from 'vitest';

import { createDocumentHost, type DocumentStore } from './document-host';

/** A stand-in for the IndexedDB-backed store, with controllable timing and failure. */
function fakeStore(initial: Record<string, string> = {}) {
  const rows = new Map(Object.entries(initial));
  const writes: Array<[string, string]> = [];
  let failNext = false;
  const store: DocumentStore & {
    writes: typeof writes;
    failNextWrite(): void;
    rows: typeof rows;
  } = {
    async loadAll() {
      return Object.fromEntries(rows);
    },
    async put(key, value) {
      if (failNext) {
        failNext = false;
        throw new Error('quota exceeded');
      }
      writes.push([key, value]);
      rows.set(key, value);
    },
    async remove(prefix) {
      for (const key of [...rows.keys()]) if (key.startsWith(prefix)) rows.delete(key);
    },
    writes,
    failNextWrite() {
      failNext = true;
    },
    rows,
  };
  return store;
}

describe('hydration', () => {
  it('makes persisted documents visible synchronously once hydrated', async () => {
    // The core reads through a synchronous call; anything not resident by then is invisible to
    // it, so hydration has to finish before the core is handed the host.
    const host = await createDocumentHost(fakeStore({ '7/title': 'kitchen' }));
    expect(host.read(7, 'title')).toBe('kitchen');
  });

  it('reports the projects it hydrated', async () => {
    const host = await createDocumentHost(fakeStore({ '7/title': 'a', '9/title': 'b' }));
    expect(host.projectIds().sort()).toEqual([7, 9]);
  });

  it('starts empty when there is nothing stored', async () => {
    const host = await createDocumentHost(fakeStore());
    expect(host.projectIds()).toEqual([]);
  });

  it('survives a store that cannot be opened at all', async () => {
    // Private browsing, blocked storage, a corrupt database: the app must still run, just
    // without remembering anything.
    const broken: DocumentStore = {
      loadAll: async () => { throw new Error('no storage'); },
      put: async () => {},
      remove: async () => {},
    };
    const host = await createDocumentHost(broken);
    expect(host.projectIds()).toEqual([]);
    expect(() => host.write(1, 'title', 'x')).not.toThrow();
  });
});

describe('reads and writes', () => {
  it('a write is visible to the very next read', async () => {
    const host = await createDocumentHost(fakeStore());
    host.write(3, 'title', 'hallway');
    expect(host.read(3, 'title')).toBe('hallway');
  });

  it('an absent document reads as undefined, never as an empty string', async () => {
    // The port turns undefined into NotFound. An empty string is indistinguishable from a
    // document written empty, and a resume would silently start from a blank plan.
    const host = await createDocumentHost(fakeStore());
    expect(host.read(3, 'missing')).toBeUndefined();
  });

  it('keeps projects separate', async () => {
    const host = await createDocumentHost(fakeStore());
    host.write(1, 'title', 'mine');
    host.write(2, 'title', 'theirs');
    expect(host.read(1, 'title')).toBe('mine');
    expect(host.read(2, 'title')).toBe('theirs');
  });

  it('removing a project drops every document under it', async () => {
    const host = await createDocumentHost(fakeStore());
    host.write(4, 'title', 'a');
    host.write(4, 'plan', 'b');
    expect(host.remove(4)).toBe(true);
    expect(host.read(4, 'title')).toBeUndefined();
    expect(host.projectIds()).toEqual([]);
  });

  it('removing something that is not there says so', async () => {
    const host = await createDocumentHost(fakeStore());
    expect(host.remove(99)).toBe(false);
  });
});

describe('persistence', () => {
  it('writes through to the store', async () => {
    const store = fakeStore();
    const host = await createDocumentHost(store);
    host.write(5, 'title', 'garden');
    await host.flush();
    expect(store.writes).toContainEqual(['5/title', 'garden']);
  });

  it('does not make the core wait for the disk', async () => {
    // write() is called from inside a synchronous C++ call. If it awaited anything, the core
    // would be blocked on storage in the middle of a capture.
    const store = fakeStore();
    const host = await createDocumentHost(store);
    host.write(5, 'title', 'garden');
    expect(store.writes.length).toBe(0);   // not yet — persistence is behind the read model
    await host.flush();
    expect(store.writes.length).toBe(1);
  });

  it('keeps serving reads when persistence fails', async () => {
    // Out of quota is common on phones. Losing durability is bad; losing the session in progress
    // is worse, so the resident copy stands and the failure is reported separately.
    const store = fakeStore();
    store.failNextWrite();
    const host = await createDocumentHost(store);
    host.write(6, 'title', 'attic');
    await host.flush();
    expect(host.read(6, 'title')).toBe('attic');
    expect(host.lastPersistError()).toMatch(/quota/i);
  });

  it('coalesces repeated writes to the same document', async () => {
    // A capture session rewrites its state constantly; persisting every intermediate value would
    // spend the storage budget on values nobody will ever read.
    const store = fakeStore();
    const host = await createDocumentHost(store);
    host.write(7, 'state', 'one');
    host.write(7, 'state', 'two');
    host.write(7, 'state', 'three');
    await host.flush();
    expect(store.writes).toEqual([['7/state', 'three']]);
  });

  it('flushing with nothing pending is harmless', async () => {
    const store = fakeStore();
    const host = await createDocumentHost(store);
    await expect(host.flush()).resolves.toBeUndefined();
    expect(store.writes.length).toBe(0);
  });

  it('schedules a flush on its own so a page that never calls flush still persists', async () => {
    vi.useFakeTimers();
    try {
      const store = fakeStore();
      const host = await createDocumentHost(store, { flushDelayMs: 50 });
      host.write(8, 'title', 'shed');
      await vi.advanceTimersByTimeAsync(60);
      expect(store.writes).toContainEqual(['8/title', 'shed']);
    } finally {
      vi.useRealTimers();
    }
  });
});
