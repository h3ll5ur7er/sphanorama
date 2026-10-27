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
    // this host, starting empty, cannot see, and knows nothing about. Kept resident instead, which
    // is what "runs without remembering" means (ADR 0078).
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
    expect(host.persistFailing()).toMatch(/quota/i);
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
    // "Is this being kept now", not "did anything ever fail": the page shows it for the life of
    // the page, so a store that fills mid-session is said, and so is its recovery (ADR 0079).
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
  });

  it('is not failing for a store that would not load, since it never tries', async () => {
    // A failed load is the boot's to say, through `loadError`; nothing is attempted after it, so
    // there is no persist to fail and nothing to hear.
    let lose: (reason: string) => void = () => {};
    const host = await createDocumentHost({
      loadAll: async () => { throw new Error('no storage'); },
      // Refusing too, so that a host which did try would be heard failing.
      put: async () => { throw new Error('no storage'); },
      remove: async () => { throw new Error('no storage'); },
      // And losing its database after, which a store whose open succeeded and whose read then
      // failed can still do: the load is what was said, and nothing more is heard.
      onLost: (listener) => { lose = listener; },
    }, { flushDelayMs: 0 });
    const heard: Array<string | null> = [];
    host.onPersistChange((failing) => heard.push(failing));
    host.write(1, 'title', 'x');
    await host.flush();
    lose('the database was closed');
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
      // Succeeding, as a persist with nothing in it does and a debounce timer outliving the write
      // it was set for runs one: what it says of the disk is nothing, since the disk is gone.
      put: async () => {},
      remove: async () => {},
      onLost: (listener) => { lose = listener; },
    }, { flushDelayMs: 0 });
    const heard: Array<string | null> = [];
    host.onPersistChange((failing) => heard.push(failing));
    lose('the database was closed');
    expect(heard).toEqual(['the database was closed']);
    expect(host.persistFailing()).toBe('the database was closed');
    // And it stays lost: a persist that goes through after it was put somewhere that is not
    // where the documents were, or nowhere at all, and does not say "saving" again.
    host.write(1, 'title', 'after');
    await host.flush();
    expect(host.persistFailing()).toBe('the database was closed');
    expect(heard).toEqual(['the database was closed']);
  });

  it('keeps saying so through a persist with nothing in it', async () => {
    // A write made while a flush runs is drained by it and leaves its debounce timer behind. That
    // timer's persist has nothing in it and goes through, after the loss.
    let lose: (reason: string) => void = () => {};
    let host: Awaited<ReturnType<typeof createDocumentHost>> | null = null;
    let writeDuring = true;
    host = await createDocumentHost({
      loadAll: async () => ({}),
      put: async () => {
        if (writeDuring) {
          writeDuring = false;
          host!.write(1, 'session', 'written during the flush');
        }
      },
      remove: async () => {},
      onLost: (listener) => { lose = listener; },
    }, { flushDelayMs: 5 });
    host.write(1, 'title', 'first');
    await host.flush();
    lose('the database was closed');
    await new Promise((resolve) => setTimeout(resolve, 30));
    expect(host.persistFailing()).toBe('the database was closed');
  });

  it('gives the loss as the reason, not the failures it causes', async () => {
    // Every persist after a loss fails against the closed database, with an error that says less
    // than the loss did about why.
    let lose: (reason: string) => void = () => {};
    let closed = false;
    const host = await createDocumentHost({
      loadAll: async () => ({}),
      put: async () => { if (closed) throw new Error('InvalidStateError'); },
      remove: async () => {},
      onLost: (listener) => { lose = listener; },
    }, { flushDelayMs: 0 });
    const heard: Array<string | null> = [];
    host.onPersistChange((failing) => heard.push(failing));
    closed = true;
    lose('the database was closed');
    host.write(1, 'title', 'refused');
    await host.flush();
    expect(host.persistFailing()).toBe('the database was closed');
    expect(heard).toEqual(['the database was closed']);
  });

  it('still drains a flush whose persists go through', async () => {
    // The flush stops at a persist that failed, which is not what a loss is: work queued during a
    // persist that went through is still taken.
    const rows = new Map<string, string>();
    let lose: (reason: string) => void = () => {};
    let host: Awaited<ReturnType<typeof createDocumentHost>> | null = null;
    let writeDuring = true;
    host = await createDocumentHost({
      loadAll: async () => ({}),
      put: async (key, value) => {
        if (writeDuring) {
          writeDuring = false;
          host!.write(1, 'late', 'written during the persist');
        }
        rows.set(key, value);
      },
      remove: async () => {},
      onLost: (listener) => { lose = listener; },
    }, { flushDelayMs: 0 });
    lose('the database was closed');
    host.write(1, 'title', 'first');
    await host.flush();
    expect(rows.get('1/late')).toBe('written during the persist');
  });
});

describe('a flush', () => {
  it('drains writes that arrive during a persist that went through', async () => {
    // The core keeps running while a persist awaits the store, so a flush can find as much queued
    // after a persist as it carried. Only a failed persist is a reason to stop, since a flush
    // that returned with work queued is answered "durable" (ADR 0079).
    // After a failure it recovered from, too: the stop is on whether this persist went through,
    // which neither a failure before it nor `persistFailing()` after a loss can say.
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
    expect(host.persistFailing()).toContain('Quota');
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

describe('issuing a project id', () => {
  // A tab's documents are a copy taken when it loaded, so the ids it can see say nothing about what
  // another tab has written since. The id is taken from the clock instead, with random low bits,
  // and kept above everything this host has held (ADR 0081).
  const SECOND = 1000;

  it('issues a tab that loaded earlier an id above one another tab issued since', async () => {
    // The overwrite this exists to close: both tabs load the same documents, the second captures,
    // and the first, which cannot see that, creates a project of its own. One past the highest
    // id either could see is the same id in both. The draws are equal, so the clock is what
    // separates them.
    const store = fakeStore({ '1/title': 'old' });
    let clock = 1_000 * SECOND;
    const options = { now: () => clock, draw: () => 7 };
    const first = await createDocumentHost(store, options);
    const second = await createDocumentHost(store, options);

    const theirs = second.issueProjectId();
    clock += SECOND;
    const mine = first.issueProjectId();
    expect(theirs).not.toBeNull();
    expect(mine).toBeGreaterThan(theirs as number);
  });

  it('keeps tabs issuing in the same second apart by the draw', async () => {
    const store = fakeStore();
    const first = await createDocumentHost(store, { now: () => 5 * SECOND, draw: () => 1 });
    const second = await createDocumentHost(store, { now: () => 5 * SECOND, draw: () => 2 });
    expect(first.issueProjectId()).not.toBe(second.issueProjectId());
  });

  it('keeps tabs apart by the draw when an id they hold is ahead of the clock', async () => {
    // A project created while the device's clock ran ahead, or before it stepped back, leaves an
    // id above what the clock now gives. Both tabs loaded it, so one past it is the same id in
    // both, which is the overwrite again; the draw has to survive the floor.
    const ahead = 2_000 * SECOND * 2 ** 20;
    const store = fakeStore({ [`${ahead}/title`]: 'from a fast clock' });
    const first = await createDocumentHost(store, { now: () => 1_000 * SECOND, draw: () => 1 });
    const second = await createDocumentHost(store, { now: () => 1_000 * SECOND, draw: () => 2 });
    const mine = first.issueProjectId() as number;
    const theirs = second.issueProjectId() as number;
    expect(mine).toBeGreaterThan(ahead);
    expect(theirs).toBeGreaterThan(ahead);
    expect(mine).not.toBe(theirs);
  });

  it('keeps tabs apart by the draw on a clock no id can come from', async () => {
    const store = fakeStore({ '5/title': 'a' });
    const first = await createDocumentHost(store, { now: () => 2 ** 60, draw: () => 1 });
    const second = await createDocumentHost(store, { now: () => 2 ** 60, draw: () => 2 });
    expect(first.issueProjectId()).not.toBe(second.issueProjectId());
  });

  it('ascends within a tab, in the same second and when the clock steps back', async () => {
    // Newest is the largest id, which is how the page picks the capture to offer back.
    let clock = 1_000 * SECOND;
    const host = await createDocumentHost(fakeStore(), { now: () => clock, draw: () => 900 });
    const first = host.issueProjectId() as number;
    const second = host.issueProjectId() as number;
    clock -= 3600 * SECOND;
    const third = host.issueProjectId() as number;
    expect(second).toBeGreaterThan(first);
    expect(third).toBeGreaterThan(second);
  });

  it('is above every id it loaded, written or removed, and never zero', async () => {
    // A clock at the epoch and a draw of zero would issue zero, which is every id's "unset". Ids an
    // older build counted out from 1 are below any the clock gives, and still held above here.
    const host = await createDocumentHost(fakeStore({ '3/title': 'counted' }),
      { now: () => 0, draw: () => 0 });
    expect(host.issueProjectId()).toBeGreaterThan(3);
    const written = 5 * 2 ** 20 + 7;
    host.write(written, 'title', 'written');
    host.remove(written);
    expect(host.issueProjectId()).toBeGreaterThan(written);
  });

  it('creates nothing', async () => {
    const host = await createDocumentHost(fakeStore());
    expect(host.issueProjectId()).toBeGreaterThan(0);
    expect(host.projectIds()).toEqual([]);
  });

  it('issues only an id a double holds exactly, and refuses when there is none', async () => {
    // A clock centuries out is a broken device, and its projects still need ids: issued above the
    // highest held instead. Past 2^53, two ids are one number, which is the overwrite again.
    const broken = await createDocumentHost(fakeStore({ '5/title': 'a' }),
      { now: () => 2 ** 60, draw: () => 0 });
    expect(broken.issueProjectId()).toBe(2 ** 20);
    // In the last second a double holds, the next second's ids are past it, so one past what is
    // held, undrawn; and past that, nothing.
    const last = Number.MAX_SAFE_INTEGER - 1;
    const nearly = await createDocumentHost(fakeStore({ [`${last}/title`]: 'a' }));
    expect(nearly.issueProjectId()).toBe(Number.MAX_SAFE_INTEGER);
    expect(nearly.issueProjectId()).toBeNull();
    const full = await createDocumentHost(fakeStore({ [`${Number.MAX_SAFE_INTEGER}/title`]: 'a' }));
    expect(full.issueProjectId()).toBeNull();
    // A key past that, which only an edited database holds, is still one the listing reports.
    const past = await createDocumentHost(fakeStore({ '1000000000000000000/title': 'a' }));
    expect(past.issueProjectId()).toBeNull();
  });

  it('draws the low bits at random by default', async () => {
    // What keeps two tabs in the same second apart, and nothing else here would see it constant.
    const store = fakeStore();
    const ids = new Set<number>();
    for (let tab = 0; tab < 16; tab += 1) {
      const host = await createDocumentHost(store, { now: () => 7 * SECOND });
      const id = host.issueProjectId() as number;
      expect(id - 7 * 2 ** 20).toBeGreaterThanOrEqual(0);
      expect(id - 7 * 2 ** 20).toBeLessThan(2 ** 20);
      ids.add(id);
    }
    expect(ids.size).toBeGreaterThan(1);
  });

  it('draws from the real clock and a random source by default', async () => {
    const before = Math.floor(Date.now() / SECOND) * 2 ** 20;
    const host = await createDocumentHost(fakeStore());
    const id = host.issueProjectId() as number;
    expect(Number.isSafeInteger(id)).toBe(true);
    expect(id).toBeGreaterThanOrEqual(before);
    expect(id).toBeLessThan(before + 2 * 2 ** 20);
  });
});
