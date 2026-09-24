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
  /** Called on `pagehide`: a page holding the right stamps its departure for its successor. */
  depart(): void;
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

export async function tierAccess(options: TierAccessOptions): Promise<TierAccess> {
  const now = options.now ?? Date.now;
  const departure = consume(options.session);
  const holder = read(options.local, HOLDER_KEY);
  const age = (at: number | undefined) => (at === undefined ? NaN : now() - at);
  const recent = (at: number | undefined, window: number) => age(at) >= 0 && age(at) < window;

  // `undefined` when the origin's record cannot be read at all: then the tab's own is all there is.
  const successor = departure !== null && recent(departure.leftAt, FRESH_MS)
    && (holder === undefined || holder?.token === departure.token);
  const handingOver = !successor && recent(holder?.leftAt, HANDOVER_MS);

  const mine = (options.token ?? (() => crypto.randomUUID()))();
  let ours = false;
  let holding = false;
  const stamp = () => {
    const left = JSON.stringify({ token: mine, leftAt: now() } satisfies Stamp);
    write(options.session, TAB_KEY, left);
    write(options.local, HOLDER_KEY, left);
  };
  const claim = () => { holding = true; write(options.local, HOLDER_KEY, JSON.stringify({ token: mine })); };
  const disclaim = () => {
    holding = false;
    if (read(options.local, HOLDER_KEY)?.token === mine) remove(options.local, HOLDER_KEY);
  };

  const { locks } = options;
  if (!locks) {
    // No Web Locks: the files are the only arbiter, as before the right existed.
    return {
      access: successor ? 'wait' : 'try',
      settle(resident) { ours = resident; },
      depart() { if (ours) stamp(); },
    };
  }
  if (handingOver) {
    // Someone left a moment ago and their successor may not have queued yet.
    return { access: 'skip', settle() {}, depart() {} };
  }

  let release = () => {};
  const held = new Promise<void>((done) => { release = done; });
  const waitMs = options.lockWaitMs ?? LOCK_WAIT_MS;
  const granted = await new Promise<boolean>((answer) => {
    const how = successor ? { signal: AbortSignal.timeout(waitMs) } : { ifAvailable: true };
    locks.request(LOCK, how, (lock) => {
      if (!lock) {
        answer(false);
        return undefined;
      }
      claim();
      answer(true);
      // Held until the worker turns out not to have the pair, or the page goes.
      return held;
    }).catch(() => {
      console.warn(`sphanorama spill: the resident pair was not handed over within ${waitMs} ms; `
        + 'taking a tier of its own');
      answer(false);
    });
  });

  return {
    access: granted ? (successor ? 'wait' : 'try') : 'skip',
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
    },
  };
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
    // The next page asks as a newcomer, which costs it only the wait.
  }
}

function remove(storageOf: () => ClaimStorage | undefined, key: string): void {
  try {
    storageOf()?.removeItem(key);
  } catch {
    // As above.
  }
}
