/**
 * The coverage map's refresh, and the four facts it has to keep straight.
 *
 * Coverage changes when a cell completes and at no other time, so `CellDone` is the only thing
 * that asks for it. That makes each read the *only* chance to record what one burst did: a read
 * that is refused, dropped, or answered from before the bank leaves a captured cell drawn as a
 * hole and `n/total` one short, for the rest of the session — for ever, if it was the last cell.
 *
 * Lifted out of `main.ts` for the reason `lock-writes.ts` was: four interacting booleans, each
 * added by a different review round to close a different window, and no way to drive any of them
 * from the composition root. A reviewer had to transcribe this function into node to measure the
 * bug in the last fix. This file is that transcription, made permanent, with the ports the page
 * fills in.
 */

export interface CoverageRefreshPorts<S> {
  /** Whether there is anything to refresh — no plan, no map, nothing to ask about. */
  enabled: () => boolean;
  /** One read. `null` is a refusal, whatever produced it: a rejection, or a `Status` that said no. */
  read: () => Promise<S | null>;
  /** What an answer means: record it, draw it. Called only for answers. */
  accept: (state: S) => void;
  /** Whether the capture loop has stopped, which is what earns the one late retry below. */
  stopped: () => boolean;
  now: () => number;
  later: (run: () => void, ms: number) => void;
  /** How long between *asks*, not between answers. */
  retryMs: number;
}

export interface CoverageRefresh {
  refresh: () => Promise<void>;
  /** Whether a refused read is owed a retry by now. The render loop asks this once a tick. */
  isDue: () => boolean;
}

export function createCoverageRefresh<S>(ports: CoverageRefreshPorts<S>): CoverageRefresh {
  // The last read was refused. `CellDone` fires once per burst and is the only thing that asks,
  // so a single refused read is permanent unless something retries it. The answer is cheap and
  // idempotent; not retrying it was the only thing making a transient failure forever.
  let stale = false;
  // When that retry may next fire. Without it, `stale` asks on every animation frame — measured at
  // 120 facade round trips in two seconds against a `coverage()` that refuses, about sixty of them
  // in flight at once. That is the same once-per-cell-into-once-per-frame mistake ADR 0041 records
  // a reviewer catching on the sibling branch, reintroduced by the fix for a dropped read.
  let retryAtMs = 0;
  // Whether one is already out. The throttle alone does not stop a *slow* refusal from being asked
  // again every frame, because it was armed when the answer came back rather than when the call
  // went out — measured at 58 calls in five seconds against a 300 ms refusal, about eighteen of
  // them overlapping, which is the same storm the throttle was added to end.
  let inFlight = false;
  // Whether the one retry a stopped loop gets has been spent. Latched rather than counted: the
  // only thing it protects against is a read refused at the moment everything else stopped.
  let lastRetried = false;
  // Something asked while a read was already out. A *different fact* from `stale`, and conflating
  // them was the whole of the last defect here: this one says a read that has not happened yet is
  // owed, `stale` says the last one failed. Setting `stale` instead is defeated by the very read
  // it waits for — the worker answers in order, so the outstanding `coverage()` was computed
  // before the cell was banked, comes back `ok`, and clears the flag on its way past. Measured:
  // `nodesSatisfied` stuck at 0 through two hundred further ticks with the core at 1.
  let askedAgain = false;

  const refresh = async (): Promise<void> => {
    if (!ports.enabled()) return;
    if (inFlight) {
      // Deferred, not dropped. A `CellDone` arriving while an earlier read is still out is the one
      // refresh that *knows* something changed, and on the last cell of a sphere there is no later
      // `CellDone` to ask again.
      askedAgain = true;
      return;
    }
    inFlight = true;
    // Armed here, not on the answer: the interval is between asks.
    retryAtMs = ports.now() + ports.retryMs;
    let state: S | null;
    try {
      state = await ports.read();
    } finally {
      // In a `finally`, because a port that *rejects* rather than answering would otherwise leave
      // this true for the life of the session: every later refresh would take the deferral branch,
      // `isDue()` would be false because `stale` was never set, and all four flags would be wedged
      // with nothing able to recover them. `read`'s contract says a refusal arrives as `null` and
      // the composition root honours that with a `.catch` — but a rejection is not this module's
      // to interpret and is still this module's to survive.
      inFlight = false;
    }

    if (state === null) {
      stale = true;
      // The deferred ask is subsumed here rather than owed. Something is going to look again
      // either way — the latched retry below when the loop has stopped, or `stale` when it is
      // still running — so carrying the flag past a refusal counts the same ask twice, and a
      // reviewer measured what that costs: the retry answers, its success issues the deferred
      // read, and the "one retry, latched" below has quietly spent two.
      askedAgain = false;
      // One retry, latched, and only once the loop has stopped: while it is running the loop does
      // this better, and if the core is what died the second attempt fails the same way and that
      // is the end of it. `inFlight` is already false here, so the retry is not blocked by the
      // read that just failed.
      //
      // It exists because `stale`'s only reader is the render loop, and the terminal branches
      // return before reaching it — so a read in flight across the camera going away that comes
      // back refused would otherwise leave the map one cell short of the work the user did.
      if (ports.stopped() && !lastRetried) {
        lastRetried = true;
        ports.later(() => { void refresh(); }, ports.retryMs);
      }
      return;
    }

    stale = false;
    ports.accept(state);

    // The refresh asked for while this one was out, issued now that it is not. Not throttled by
    // `retryAtMs`, deliberately: that interval spaces *retries of a refusal*, and this is a read
    // somebody asked for because something changed.
    //
    // Bounded at one extra read per completed one: the follow-up starts with nothing in flight, so
    // it cannot re-defer, and only a fresh ask from outside can set the flag again — while a
    // `CellDone` arrives at most once a cell. (Clearing it before the call rather than after is
    // the same program either way, which a sabotage showed; it is written this way because it
    // stays the same program if `refresh` ever grows a synchronous path back to the deferral.)
    if (askedAgain) {
      askedAgain = false;
      void refresh();
    }
  };

  return {
    refresh,
    isDue: () => stale && ports.now() >= retryAtMs,
  };
}
