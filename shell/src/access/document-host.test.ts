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
    expect(host.loadError()).toContain('no storage');
  });

  it('never writes over a store it could not load', async () => {
    // What it could not load may still be there: a capture whose documents are on disk and that
    // this host, starting empty, cannot see. Project ids restart at 1 against an empty listing, so
    // persisting behind would put a new capture's title and session over the old one's. Kept
    // resident instead, which is what "runs without remembering" means (ADR 0078).
    let writes = 0;
    let removals = 0;
    const unreadable: DocumentStore = {
      loadAll: async () => { throw new Error('version error'); },
      put: async () => { writes += 1; },
      remove: async () => { removals += 1; },
    };
    const host = await createDocumentHost(unreadable, { flushDelayMs: 0 });
    host.write(1, 'title', 'new');
    host.write(1, 'session', 'sphanorama-session 3');
    host.remove(1);
    host.write(2, 'title', 'other');
    await host.flush();
    // And through the debounce, which is how almost every real write persists. `flush` cancels
    // the timer the writes above set, so one more write after it sets a timer nothing cancels, and
    // a host that refused only in `flush` writes when it fires. A removal too, since removing a
    // project the host could not see would delete the very capture this is protecting.
    host.write(3, 'title', 'after');
    host.remove(3);
    await new Promise((resolve) => setTimeout(resolve, 10));
    expect(writes).toBe(0);
    expect(removals).toBe(0);
    expect(host.read(2, 'title')).toBe('other');
  });

  it('reports no load error for a store that loaded', async () => {
    const host = await createDocumentHost(fakeStore({ '7/title': 'kitchen' }));
    expect(host.loadError()).toBeNull();
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

describe('whether it is saving now', () => {
  it('says when persisting starts failing and when it recovers, and only then', async () => {
    // `lastPersistError()` holds on to the last failure for good, which is the right answer to
    // "did anything ever fail" and the wrong one to "is this being kept now". The page shows the
    // second, for the life of the page, so a store that fills mid-session is said (ADR 0079).
    const store = fakeStore();
    const host = await createDocumentHost(store, { flushDelayMs: 0 });
    const heard: Array<string | null> = [];
    host.onPersistChange((failing) => heard.push(failing));
    expect(host.persistFailing()).toBeNull();

    host.write(1, 'title', 'kept');
    await host.flush();
    expect(heard).toEqual([]);

    store.failNextWrite();
    host.write(1, 'title', 'refused');
    await host.flush();
    expect(heard).toHaveLength(1);
    expect(heard[0]).toContain('quota exceeded');
    expect(host.persistFailing()).toContain('quota exceeded');

    // And failing again is not news: a sustained outage is one change, not a flicker of
    // "saving" between each refused persist.
    store.failNextWrite();
    host.write(1, 'title', 'refused again');
    await host.flush();
    expect(heard).toHaveLength(1);
    expect(host.persistFailing()).toContain('quota exceeded');

    host.write(1, 'title', 'kept again');
    await host.flush();
    expect(heard).toHaveLength(2);
    expect(heard[1]).toBeNull();
    expect(host.persistFailing()).toBeNull();
    // The history is not the state: the last failure is still reported as having happened.
    expect(host.lastPersistError()).toContain('quota exceeded');
  });

  it('is not failing for a store that would not load, since it never tries', async () => {
    // A failed load is the boot's to say, through `loadError`; nothing is attempted after it, so
    // there is no persist to fail and nothing to hear.
    const host = await createDocumentHost({
      loadAll: async () => { throw new Error('no storage'); },
      // Refusing too, so that a host which did try would be heard failing.
      put: async () => { throw new Error('no storage'); },
      remove: async () => { throw new Error('no storage'); },
    }, { flushDelayMs: 0 });
    const heard: Array<string | null> = [];
    host.onPersistChange((failing) => heard.push(failing));
    host.write(1, 'title', 'x');
    await host.flush();
    expect(heard).toEqual([]);
    expect(host.persistFailing()).toBeNull();
  });
});

describe('a store that loses its database', () => {
  it('says it is not saving at once, without waiting for a write to find out', async () => {
    // A clear of the site's data closes the database under the page, and what was on disk goes
    // with it. Nothing may be written for a while after, a capture being reviewed writes nothing,
    // so the persist that would have found out is not coming (ADR 0079).
    let lose: (reason: string) => void = () => {};
    const host = await createDocumentHost({
      loadAll: async () => ({ '1/title': 'kept' }),
      put: async () => { throw new Error('closed'); },
      remove: async () => { throw new Error('closed'); },
      onLost: (listener) => { lose = listener; },
    }, { flushDelayMs: 0 });
    const heard: Array<string | null> = [];
    host.onPersistChange((failing) => heard.push(failing));
    lose('the database was closed');
    expect(heard).toEqual(['the database was closed']);
    expect(host.persistFailing()).toBe('the database was closed');
    await host.flush();
    expect(host.persistFailing()).not.toBeNull();
  });
});

describe('a flush', () => {
  it('drains writes that arrive during a persist that went through', async () => {
    // The core keeps running while a persist awaits the store, so a flush can find as much queued
    // after a persist as it carried. Only a failed persist is a reason to stop, since a flush
    // that returned with work queued is answered "durable" (ADR 0079).
    // After a failure it recovered from, too: the stop is on whether persisting is failing now,
    // which the history of failures would answer wrongly for the rest of the page.
    const rows = new Map<string, string>();
    let host: Awaited<ReturnType<typeof createDocumentHost>> | null = null;
    let failNext = true;
    let writeDuring = false;
    host = await createDocumentHost({
      loadAll: async () => ({}),
      put: async (key, value) => {
        if (failNext) {
          failNext = false;
          throw new Error('quota exceeded');
        }
        if (writeDuring) {
          writeDuring = false;
          host!.write(1, 'session', 'written while the title persisted');
        }
        rows.set(key, value);
      },
      remove: async () => {},
    }, { flushDelayMs: 0 });
    host.write(1, 'title', 'refused');
    await host.flush();
    expect(host.persistFailing()).not.toBeNull();
    writeDuring = true;
    host.write(1, 'title', 'first');
    await host.flush();
    expect(rows.get('1/session')).toBe('written while the title persisted');
    expect(host.persistFailing()).toBeNull();
  });
});

describe('a listener that throws', () => {
  it('does not cost the writes that failed, nor the persists after it', async () => {
    // Listeners are the page's business; durability is the host's. One that throws must not turn
    // a failed persist into a lost one.
    const store = fakeStore();
    const host = await createDocumentHost(store, { flushDelayMs: 0 });
    host.onPersistChange(() => { throw new Error('listener broke'); });
    // Nor cost the listeners after it the news.
    const heard: Array<string | null> = [];
    host.onPersistChange((failing) => heard.push(failing));
    store.failNextWrite();
    host.write(1, 'title', 'kept in the end');
    await host.flush();
    host.write(2, 'title', 'next');
    await host.flush();
    expect(store.rows.get('1/title')).toBe('kept in the end');
    expect(store.rows.get('2/title')).toBe('next');
    expect(heard).toHaveLength(2);
    expect(heard[1]).toBeNull();
  });
});

describe('a project removed while its persist is failing', () => {
  it('is not written back by the retry', async () => {
    // The retry puts back what failed unless something newer is queued for the key, and a removal
    // is the other thing that can happen since: put back, the next persist removed the project and
    // then wrote its title again, so a deleted project came back on disk with nothing but a title.
    let release: () => void = () => {};
    const gate = new Promise<void>((resolve) => { release = resolve; });
    let entered: () => void = () => {};
    const parkedInPut = new Promise<void>((resolve) => { entered = resolve; });
    let parkNext = true;
    const rows = new Map<string, string>();
    const store: DocumentStore = {
      loadAll: async () => ({}),
      put: async (key, value) => {
        if (parkNext) {
          parkNext = false;
          entered();
          await gate;
          throw new Error('quota exceeded');
        }
        rows.set(key, value);
      },
      remove: async (prefix) => {
        for (const key of [...rows.keys()]) if (key.startsWith(prefix)) rows.delete(key);
      },
    };
    const host = await createDocumentHost(store, { flushDelayMs: 0 });
    host.write(1, 'title', 'doomed');
    const parked = host.flush();
    await parkedInPut;
    host.remove(1);
    release();
    await parked;
    await host.flush();
    await new Promise((resolve) => setTimeout(resolve, 10));
    expect(host.projectIds()).toEqual([]);
    expect([...rows.keys()]).toEqual([]);
  });
});

describe('durability under failure and concurrency', () => {
  it('keeps failed writes pending so a later flush can retry them', async () => {
    // Pending work was cleared before the write was attempted and never put back, so one
    // transient quota error meant the resident state could never become durable again — the
    // caller has no way to know which documents to rewrite.
    let failing = true;
    const written = new Map<string, string>();
    const store: DocumentStore = {
      loadAll: async () => ({}),
      put: async (key, value) => {
        if (failing) throw new Error('QuotaExceededError');
        written.set(key, value);
      },
      remove: async () => {},
    };

    const host = await createDocumentHost(store, { flushDelayMs: 0 });
    host.write(1, 'title', 'kitchen');
    await host.flush();
    expect(host.lastPersistError()).toContain('Quota');
    expect(written.size).toBe(0);

    failing = false;
    await host.flush();
    expect(written.get('1/title')).toBe('kitchen');
  });

  it('does not let a retry undo a newer value', async () => {
    // The retry carries the value that failed. If a newer write landed in the meantime, putting
    // the old one back must not overwrite it.
    let failing = true;
    const written = new Map<string, string>();
    const store: DocumentStore = {
      loadAll: async () => ({}),
      put: async (key, value) => {
        if (failing) throw new Error('nope');
        written.set(key, value);
      },
      remove: async () => {},
    };

    const host = await createDocumentHost(store, { flushDelayMs: 0 });
    host.write(1, 'title', 'old');
    await host.flush();
    host.write(1, 'title', 'new');
    failing = false;
    await host.flush();

    expect(written.get('1/title')).toBe('new');
    expect(host.read(1, 'title')).toBe('new');
  });

  it('never runs two persists at once', async () => {
    // The timer-driven path is where they overlap: a write arriving while an earlier persist is
    // still running schedules another one, which replaces the in-flight handle rather than
    // queueing behind it. Two snapshots then race, and the slower older one can finish last and
    // restore stale data over the newer one — while flush() awaits only whichever handle it
    // happened to see.
    let running = 0;
    let overlapped = false;
    const store: DocumentStore = {
      loadAll: async () => ({}),
      put: async () => {
        running += 1;
        if (running > 1) overlapped = true;
        await new Promise((resolve) => setTimeout(resolve, 20));
        running -= 1;
      },
      remove: async () => {},
    };

    const host = await createDocumentHost(store, { flushDelayMs: 1 });
    host.write(1, 'a', '1');
    await new Promise((resolve) => setTimeout(resolve, 5));   // the first persist is now running
    host.write(1, 'b', '2');                                  // schedules a second one behind it
    await new Promise((resolve) => setTimeout(resolve, 10));  // which fires mid-flight
    await host.flush();
    expect(overlapped).toBe(false);
  });

  it('flush waits for work queued while an earlier flush was running', async () => {
    const written = new Map<string, string>();
    const store: DocumentStore = {
      loadAll: async () => ({}),
      put: async (key, value) => {
        await new Promise((resolve) => setTimeout(resolve, 5));
        written.set(key, value);
      },
      remove: async () => {},
    };

    const host = await createDocumentHost(store, { flushDelayMs: 0 });
    host.write(1, 'a', '1');
    const pending = host.flush();
    host.write(1, 'b', '2');
    await pending;
    await host.flush();
    expect(written.get('1/a')).toBe('1');
    expect(written.get('1/b')).toBe('2');
  });
});
