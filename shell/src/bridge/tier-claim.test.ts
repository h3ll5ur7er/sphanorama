import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { FRESH_MS, HANDOVER_MS, type LockManagerLike, tierAccess } from './tier-claim';

const TAB_KEY = 'sphanorama-resident-tier';
const HOLDER_KEY = 'sphanorama-resident-holder';
const LOCK = 'sphanorama-spill-resident';

function memoryStorage() {
  const items = new Map<string, string>();
  return {
    items,
    getItem: (key: string) => items.get(key) ?? null,
    setItem: (key: string, value: string) => { items.set(key, value); },
    removeItem: (key: string) => { items.delete(key); },
  };
}

// The Web Locks behaviour the right depends on: one holder, waiters granted in order the moment it
// goes, `ifAvailable` answered at once without joining the queue, an aborted waiter removed, and a
// holder that dies with its page.
function fakeLocks() {
  let held = false;
  let dies = () => {};
  const queue: Array<() => void> = [];
  const grant = (callback: (lock: { name: string } | null) => unknown) => {
    held = true;
    const death = new Promise<void>((done) => { dies = done; });
    return Promise.race([Promise.resolve(callback({ name: LOCK })), death]).finally(() => {
      held = false;
      queue.shift()?.();
    });
  };
  const locks: LockManagerLike & { readonly held: boolean; holderDies(): Promise<void> } = {
    get held() { return held; },
    // Resolved once the release has gone through, as it has by the time a later page asks.
    holderDies: async () => { dies(); await new Promise((done) => setTimeout(done, 0)); },
    request(_name, options, callback) {
      if (options.ifAvailable) return held ? Promise.resolve(callback(null)) : grant(callback);
      if (!held) return grant(callback);
      return new Promise((resolve, reject) => {
        const entry = () => { grant(callback).then(resolve, reject); };
        queue.push(entry);
        options.signal?.addEventListener('abort', () => {
          const at = queue.indexOf(entry);
          if (at < 0) return;
          queue.splice(at, 1);
          reject(new DOMException('aborted', 'AbortError'));
        });
      });
    },
  };
  return locks;
}

// One origin: its locks and its localStorage, shared by every tab; each tab has its own session.
function origin() {
  const locks = fakeLocks();
  const local = memoryStorage();
  let clock = 1_000_000;
  let tokens = 0;
  const open = (tab: ReturnType<typeof memoryStorage>, lockWaitMs?: number) => tierAccess({
    session: () => tab, local: () => local, locks, now: () => clock,
    token: () => `page-${++tokens}`, lockWaitMs,
  });
  // A page that has stamped its departure but whose lock has not been let go yet — its release still
  // on the way, or a page that died without `pagehide` after an earlier stamp.
  const departedStillHolding = (tab: ReturnType<typeof memoryStorage>) => {
    const left = JSON.stringify({ token: 'old-page', leftAt: clock });
    tab.setItem(TAB_KEY, left);
    local.setItem(HOLDER_KEY, left);
    void locks.request(LOCK, {}, () => new Promise(() => {}));
  };
  return { locks, local, open, tab: memoryStorage, departedStillHolding, advance: (ms: number) => { clock += ms; } };
}

const pendingAfter = async <T>(promise: Promise<T>) =>
  Promise.race([promise, new Promise<'pending'>((done) => setTimeout(() => done('pending'), 20))]);

describe("a page's right to the resident spill tier", () => {
  let warn: ReturnType<typeof vi.spyOn>;
  beforeEach(() => { warn = vi.spyOn(console, 'warn').mockImplementation(() => {}); });
  afterEach(() => { warn.mockRestore(); });

  it('is taken by the first page, which then holds it', async () => {
    const o = origin();
    expect((await o.open(o.tab())).access).toBe('try');
    expect(o.locks.held).toBe(true);
  });

  it('is handed to the page that replaces its holder, even while that holder is slow to go', async () => {
    const o = origin();
    const tab = o.tab();
    o.departedStillHolding(tab);
    o.advance(300);

    const successor = o.open(tab);
    expect(await pendingAfter(successor)).toBe('pending');
    await o.locks.holderDies();
    expect((await successor).access).toBe('wait');
  });

  it('is handed to the successor when the holder was already gone', async () => {
    const o = origin();
    const tab = o.tab();
    (await o.open(tab)).depart();
    await o.locks.holderDies();
    o.advance(300);
    expect((await o.open(tab)).access).toBe('wait');
  });

  it('is not taken by a newcomer in the moment between a departure and its successor', async () => {
    // The race every polling rule lost: a new tab asking just as the old page goes.
    const o = origin();
    const tab = o.tab();
    (await o.open(tab)).depart();
    await o.locks.holderDies();
    o.advance(10);

    expect((await o.open(o.tab())).access).toBe('skip');
    expect((await o.open(tab)).access).toBe('wait');
  });

  it('is taken by a newcomer once a departure is old enough that no successor is coming', async () => {
    const o = origin();
    (await o.open(o.tab())).depart();
    await o.locks.holderDies();
    o.advance(HANDOVER_MS);
    expect((await o.open(o.tab())).access).toBe('try');
  });

  it('is not claimed by a tab back at the app after someone else took it over', async () => {
    // Left, a new tab took the right, came back within seconds: queueing would put it ahead of the
    // live holder's own reload.
    const o = origin();
    const returning = o.tab();
    (await o.open(returning)).depart();
    await o.locks.holderDies();
    o.advance(HANDOVER_MS);
    expect((await o.open(o.tab())).access).toBe('try');
    o.advance(500);

    const back = o.open(returning);
    // Answered at once rather than after queueing and giving up, which would also end in 'skip'.
    const answer = await pendingAfter(back);
    expect(answer === 'pending' ? answer : answer.access).toBe('skip');
  });

  it('is not claimed on a departure older than a reload takes', async () => {
    const o = origin();
    const tab = o.tab();
    (await o.open(tab)).depart();
    await o.locks.holderDies();
    o.advance(FRESH_MS);
    expect((await o.open(tab)).access).toBe('try');
  });

  it('is not waited for by a duplicated tab, which copies nothing to succeed with', async () => {
    const o = origin();
    const original = o.tab();
    const firstPage = await o.open(original);
    expect(firstPage.access).toBe('try');
    // What Duplicate Tab copies is whatever the live original's session holds, which is nothing.
    const duplicate = o.tab();
    for (const [key, value] of original.items) duplicate.items.set(key, value);
    const answer = await pendingAfter(o.open(duplicate));
    expect(answer === 'pending' ? answer : answer.access).toBe('skip');
  });

  it('is given up on after a while by a successor whose holder never goes', async () => {
    const o = origin();
    const tab = o.tab();
    o.departedStillHolding(tab);
    o.advance(300);
    expect((await o.open(tab, 30)).access).toBe('skip');
    expect(warn).toHaveBeenCalledWith(expect.stringContaining('not handed over within 30 ms'));
  });

  it('is let go by a page whose worker did not get the resident tier, and kept by one that did', async () => {
    for (const resident of [true, false]) {
      const o = origin();
      (await o.open(o.tab())).settle(resident);
      await pendingAfter(Promise.resolve());
      expect(o.locks.held).toBe(resident);
      expect(o.local.items.has(HOLDER_KEY)).toBe(resident);
    }
  });

  it('leaves a departure only when this page held the right', async () => {
    const o = origin();
    const holding = o.tab();
    const holder = await o.open(holding);

    // Skipped because the first page holds the right, not because anyone has left.
    const skipped = o.tab();
    const bystander = await o.open(skipped);
    expect(bystander.access).toBe('skip');
    bystander.depart();
    expect(skipped.items.has(TAB_KEY)).toBe(false);

    holder.depart();
    expect(JSON.parse(holding.items.get(TAB_KEY)!)).toMatchObject({ token: 'page-1' });
  });

  it('forgets a departure as soon as it is read', async () => {
    const o = origin();
    const tab = o.tab();
    (await o.open(tab)).depart();
    await o.locks.holderDies();
    await o.open(tab);
    expect(tab.items.has(TAB_KEY)).toBe(false);
  });

  it('falls back to the files alone where there is no Web Locks API', async () => {
    const session = memoryStorage();
    const local = memoryStorage();
    const open = () => tierAccess({ session: () => session, local: () => local, locks: undefined, now: () => 5 });
    const first = await open();
    expect(first.access).toBe('try');
    first.depart();
    expect(session.items.has(TAB_KEY)).toBe(false);
    first.settle(true);
    first.depart();
    expect((await open()).access).toBe('wait');
  });

  it('trusts the tab alone where the origin record cannot be read', async () => {
    const o = origin();
    const tab = o.tab();
    (await o.open(tab)).depart();
    await o.locks.holderDies();
    const decided = await tierAccess({
      session: () => tab, local: () => { throw new DOMException('denied', 'SecurityError'); },
      locks: o.locks, now: () => 1_000_000,
    });
    expect(decided.access).toBe('wait');
  });

  it('does without crypto.randomUUID, which an insecure context lacks', async () => {
    // Exactly where the no-Web-Locks fallback runs; a missing one used to stop the core loading.
    vi.stubGlobal('crypto', {});
    try {
      const session = memoryStorage();
      const decided = await tierAccess({ session: () => session, local: () => memoryStorage(), locks: undefined });
      decided.settle(true);
      decided.depart();
      expect(JSON.parse(session.items.get(TAB_KEY)!).token).toEqual(expect.any(String));
    } finally {
      vi.unstubAllGlobals();
    }
  });

  it('does without AbortSignal.timeout, which arrived after Web Locks', async () => {
    const o = origin();
    const tab = o.tab();
    o.departedStillHolding(tab);
    o.advance(300);
    const original = AbortSignal.timeout;
    (AbortSignal as unknown as { timeout?: unknown }).timeout = undefined;
    try {
      expect((await o.open(tab, 30)).access).toBe('skip');
    } finally {
      AbortSignal.timeout = original;
    }
  });

  it('is let go on departure, so the page can go into the back/forward cache', async () => {
    // Chromium never caches a page holding a Web Lock; the departure keeps newcomers off instead.
    const o = origin();
    const tab = o.tab();
    const page = await o.open(tab);
    page.settle(true);
    page.depart();
    await pendingAfter(Promise.resolve());
    expect(o.locks.held).toBe(false);
    expect(JSON.parse(tab.items.get(TAB_KEY)!)).toMatchObject({ token: 'page-1' });
  });

  it('is taken back by a page restored from the cache with the pair, and not by one without it', async () => {
    for (const resident of [true, false]) {
      const o = origin();
      const tab = o.tab();
      const page = await o.open(tab);
      page.settle(resident);
      page.depart();
      await pendingAfter(Promise.resolve());

      page.resume();
      await pendingAfter(Promise.resolve());
      expect(o.locks.held).toBe(resident);
      expect(tab.items.has(TAB_KEY)).toBe(false);
      if (resident) expect(JSON.parse(o.local.items.get(HOLDER_KEY)!)).toEqual({ token: 'page-1' });
    }
  });

  it('is queued for by a restored page whose right a newcomer took but has not used yet', async () => {
    // Once a newcomer's worker tries the files, Chromium evicts the cached page and it never comes
    // back. This is the order in which the page is restored first: the newcomer holds the right and
    // is still booting, and its try then fails on the files the restored page's worker holds. The
    // restored page must queue behind it rather than give up, and must not claim the record early.
    const o = origin();
    const cached = await o.open(o.tab());
    cached.settle(true);
    cached.depart();
    await pendingAfter(Promise.resolve());
    o.advance(HANDOVER_MS);
    const newcomer = await o.open(o.tab());
    expect(newcomer.access).toBe('try');

    cached.resume();
    await pendingAfter(Promise.resolve());
    expect(JSON.parse(o.local.items.get(HOLDER_KEY)!)).toEqual({ token: 'page-2' });
    newcomer.settle(false);
    await pendingAfter(Promise.resolve());
    expect(o.locks.held).toBe(true);
    expect(JSON.parse(o.local.items.get(HOLDER_KEY)!)).toEqual({ token: 'page-1' });
  });

  it('never stops the page loading: anything thrown on the way leaves the files to decide', async () => {
    const throwing: LockManagerLike = { request() { throw new TypeError('no locks here'); } };
    const decided = await tierAccess({ session: () => memoryStorage(), local: () => memoryStorage(), locks: throwing });
    expect(decided.access).toBe('try');
    expect(warn).toHaveBeenCalledWith(expect.stringContaining('could not decide'));
  });

  it('leaves the files to decide when the lock manager refuses outright, rather than the pair to nobody', async () => {
    // What Chromium is expected to do with Web Locks where storage is blocked. Skipping would leave
    // every page, the successor included, off a pair nobody holds.
    const refusing: LockManagerLike = {
      request: () => Promise.reject(new DOMException('storage is blocked', 'SecurityError')),
    };
    const o = origin();
    const tab = o.tab();
    (await o.open(tab)).depart();
    await o.locks.holderDies();
    const open = (session: ReturnType<typeof memoryStorage>, at: number) => tierAccess({
      session: () => session, local: () => o.local, locks: refusing, now: () => at,
    });
    expect((await open(tab, 1_000_000)).access).toBe('wait');
    expect((await open(o.tab(), 1_000_000 + HANDOVER_MS)).access).toBe('try');
    expect(warn).toHaveBeenCalledWith(expect.stringContaining('refused (SecurityError'));
  });

  it('counts nothing from the future, which a clock set back would make of every stamp', async () => {
    const o = origin();
    const tab = o.tab();
    (await o.open(tab)).depart();
    await o.locks.holderDies();
    o.advance(-60_000);
    expect((await o.open(o.tab())).access).toBe('try');
    expect((await o.open(tab)).access).toBe('skip');
  });

  it('does not make a successor of a departure dated in the future', async () => {
    // The departure names the holder the origin still has, so only the clock can refuse it.
    const o = origin();
    const tab = o.tab();
    (await o.open(tab)).depart();
    await o.locks.holderDies();
    o.advance(-60_000);
    expect((await o.open(tab)).access).toBe('try');
  });

  it('is not claimed by a duplicate of a successor, even where only the tab can be read', async () => {
    // With the origin's record unreadable, removing the departure on reading is the only guard.
    const o = origin();
    const tab = o.tab();
    (await o.open(tab)).depart();
    await o.locks.holderDies();
    const noOrigin = () => { throw new DOMException('denied', 'SecurityError'); };
    const open = (session: ReturnType<typeof memoryStorage>) => tierAccess({
      session: () => session, local: noOrigin, locks: o.locks, now: () => 1_000_000, lockWaitMs: 30,
    });
    expect((await open(tab)).access).toBe('wait');

    const duplicate = o.tab();
    for (const [key, value] of tab.items) duplicate.items.set(key, value);
    const answer = await pendingAfter(open(duplicate));
    expect(answer === 'pending' ? answer : answer.access).toBe('skip');
  });

  it('treats storage it cannot reach as no departure, as Chromium throws when storage is off', async () => {
    const unreachable = () => { throw new DOMException('denied', 'SecurityError'); };
    const decided = await tierAccess({ session: unreachable, local: unreachable, locks: fakeLocks() });
    expect(decided.access).toBe('try');
    expect(() => decided.depart()).not.toThrow();
    expect(() => decided.settle(false)).not.toThrow();
  });
});
