import type { ResidentAccess } from '../access/spill-host';

/**
 * Which page may take the resident spill pair, decided before its worker asks for it (ADR 0063).
 *
 * Polling a held file cannot say who should have it next: whoever asks first after it comes free
 * gets it, and every rule for who may poll was measured handing the pair to the wrong session.
 * So the right to the pair is a Web Lock, held by the page for its whole life. A lock is handed to
 * the first waiter the moment its holder goes, so a page that queues for it is next without having
 * to be quick, and a page that only asks `ifAvailable` can never jump that queue.
 *
 * Only the holder's successor queues: the next page in the tab whose page held the right, and only
 * if nobody has taken the right since. Two records say so. The tab's own (`sessionStorage`) is the
 * departure this tab's holder stamped on `pagehide`; the origin's (`localStorage`) names the page
 * holding the right now, and when it left if it has. A successor is a page whose tab's departure is
 * fresh and names the holder the origin still has. Everything else — a new tab, a duplicated one, a
 * tab back at the app after someone else took over — asks only if the right is free, and stands
 * aside for a moment after any departure, so it cannot slip in before the successor has queued.
 *
 * With the right in hand the worker may still find the files held, because a busy old worker lets
 * go of them up to about two seconds after its page, so it polls them; nobody else can take them in
 * that gap.
 */
export interface TierAccess {
  access: ResidentAccess;
  /** Called with whether the worker got the resident pair; a page that did not gives up the right. */
  settle(resident: boolean): void;
  /** Called on `pagehide`: a page holding the right stamps its departure and lets the right go. */
  depart(): void;
  /** Called on `pageshow` from the back/forward cache: a page whose worker has the pair takes the right back. */
  resume(): void;
}

type ClaimStorage = Pick<Storage, 'getItem' | 'setItem' | 'removeItem'>;

interface LockLike { name: string }
export interface LockManagerLike {
  request(name: string, options: { ifAvailable?: boolean; signal?: AbortSignal },
          callback: (lock: LockLike | null) => unknown): Promise<unknown>;
}

export interface TierAccessOptions {
  /** Accessors, because switched-off storage makes Chromium throw from the property itself. */
  session: () => ClaimStorage | undefined;
  local: () => ClaimStorage | undefined;
  locks: LockManagerLike | undefined;
  now?: () => number;
  token?: () => string;
  /** How long a successor queues for the right before taking a tier of its own. */
  lockWaitMs?: number;
}

const TAB_KEY = 'sphanorama-resident-tier';
const HOLDER_KEY = 'sphanorama-resident-holder';
const LOCK = 'sphanorama-spill-resident';
// A reload reaches this within a second of its departure; the old worker is gone by about two.
export const FRESH_MS = 5000;
// How long a newcomer stands aside after a departure, for the successor to queue. Also how soon a
// closed app's tab can be reopened onto its pair, which is why it is not FRESH_MS.
export const HANDOVER_MS = 2000;
export const LOCK_WAIT_MS = 3000;

interface Stamp { token: string; leftAt?: number }

const FALLBACK: TierAccess = { access: 'try', settle() {}, depart() {}, resume() {} };

/**
 * Decides this page's access, and never stops the page loading over it: anything that throws on
 * the way leaves the files as the only arbiter, which is what every page had before the right.
 */
export async function tierAccess(options: TierAccessOptions): Promise<TierAccess> {
  try {
    return await decide(options);
  } catch (cause) {
    console.warn(`sphanorama spill: could not decide the right to the resident pair (${String(cause)}); `
      + 'trying the files once');
    return FALLBACK;
  }
}

async function decide(options: TierAccessOptions): Promise<TierAccess> {
  const now = options.now ?? Date.now;
  const departure = consume(options.session);
  const holder = read(options.local, HOLDER_KEY);
  const age = (at: number | undefined) => (at === undefined ? NaN : now() - at);
  // Never counted from the future: a clock set back would otherwise make a departure fresh for as
  // long as it takes to catch up, and every newcomer stand aside for as long.
  const recent = (at: number | undefined, window: number) => age(at) >= 0 && age(at) < window;

  // `undefined` when the origin's record cannot be read at all: then the tab's own is all there is.
  const successor = departure !== null && recent(departure.leftAt, FRESH_MS)
    && (holder === undefined || holder?.token === departure.token);
  const handingOver = !successor && recent(holder?.leftAt, HANDOVER_MS);

  const mine = (options.token ?? newToken)();
  let ours = false;
  let holding = false;
  let release = () => {};
  const stamp = () => {
    const left = JSON.stringify({ token: mine, leftAt: now() } satisfies Stamp);
    write(options.session, TAB_KEY, left);
    write(options.local, HOLDER_KEY, left);
  };
  const claim = () => { write(options.local, HOLDER_KEY, JSON.stringify({ token: mine })); };
  const disclaim = () => {
    if (read(options.local, HOLDER_KEY)?.token === mine) remove(options.local, HOLDER_KEY);
  };

  const { locks } = options;
  if (!locks) {
    // No Web Locks: the files are the only arbiter, as before the right existed.
    return {
      access: successor ? 'wait' : 'try',
      settle(resident) { ours = resident; },
      depart() { if (ours) stamp(); },
      resume() { remove(options.session, TAB_KEY); },
    };
  }
  if (handingOver) {
    // Someone left a moment ago and their successor may not have queued yet.
    return { access: 'skip', settle() {}, depart() {}, resume() {} };
  }

  const waitMs = options.lockWaitMs ?? LOCK_WAIT_MS;
  // Held until the worker turns out not to have the pair, or the page leaves.
  type Outcome = 'granted' | 'held elsewhere' | 'refused';
  const take = (how: { ifAvailable?: boolean; signal?: AbortSignal }) => new Promise<Outcome>((answer) => {
    locks.request(LOCK, how, (lock) => {
      if (!lock) {
        answer('held elsewhere');
        return undefined;
      }
      holding = true;
      claim();
      answer('granted');
      return new Promise<void>((done) => { release = () => { holding = false; done(); }; });
    }).catch((cause: unknown) => {
      const timedOut = (cause as { name?: unknown } | null)?.name === 'AbortError';
      console.warn(timedOut
        ? `sphanorama spill: the resident pair was not handed over within ${waitMs} ms; taking a tier of its own`
        : `sphanorama spill: the right to the resident pair was refused (${String(cause)}); the files alone decide`);
      answer(timedOut ? 'held elsewhere' : 'refused');
    });
  });
  const outcome = await take(successor ? { signal: timeout(waitMs) } : { ifAvailable: true });
  // A lock manager that refuses outright is the same as none: the files decide, as before the right
  // existed. Skipping instead would leave the pair to nobody, the successor included.
  const asIfNoLocks = outcome === 'refused';

  return {
    access: outcome === 'held elsewhere' ? 'skip' : successor ? 'wait' : 'try',
    settle(resident) {
      ours = resident;
      if (!resident && holding) {
        disclaim();
        release();
      }
    },
    depart() {
      // Holding the right before the worker has answered counts: its worker may have the files.
      if (holding || ours) stamp();
      // Let go now rather than when the page dies: a page holding a Web Lock is never put in the
      // back/forward cache. The departure just stamped keeps newcomers off until the successor has
      // queued, and it is all that protects a page sitting in the cache: after HANDOVER_MS a
      // newcomer that tries the files makes Chromium evict the cached page and gets the pair.
      release();
    },
    resume() {
      // Back from the back/forward cache with the worker, and the files, it left with.
      remove(options.session, TAB_KEY);
      if (ours && !holding && !asIfNoLocks) void take({});
    },
  };
}

// Neither is everywhere Web Locks and storage are: `crypto.randomUUID` is secure-context only, which
// is exactly where the no-Web-Locks fallback runs, and `AbortSignal.timeout` arrived after Web Locks
// in both Safari and Chrome. Either missing used to stop the core loading.
function newToken(): string {
  return globalThis.crypto?.randomUUID?.() ?? `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

function timeout(ms: number): AbortSignal {
  if (typeof AbortSignal.timeout === 'function') return AbortSignal.timeout(ms);
  const controller = new AbortController();
  setTimeout(() => controller.abort(), ms);
  return controller.signal;
}

// Read once and removed at once, so a tab duplicated later copies nothing to succeed with.
function consume(storageOf: () => ClaimStorage | undefined): Stamp | null {
  const stamp = read(storageOf, TAB_KEY);
  remove(storageOf, TAB_KEY);
  return stamp ?? null;
}

function read(storageOf: () => ClaimStorage | undefined, key: string): Stamp | null | undefined {
  try {
    const storage = storageOf();
    if (!storage) return undefined;
    const value = storage.getItem(key);
    if (value === null) return null;
    const parsed = JSON.parse(value) as Partial<Stamp>;
    return typeof parsed.token === 'string' ? { token: parsed.token, leftAt: parsed.leftAt } : null;
  } catch {
    // Unreadable storage and an unreadable record alike: nothing to go on.
    return undefined;
  }
}

function write(storageOf: () => ClaimStorage | undefined, key: string, value: string): void {
  try {
    storageOf()?.setItem(key, value);
  } catch {
    // The next page asks as a newcomer: a successor that cannot be recognised loses the resume.
  }
}

function remove(storageOf: () => ClaimStorage | undefined, key: string): void {
  try {
    storageOf()?.removeItem(key);
  } catch {
    // As above.
  }
}
