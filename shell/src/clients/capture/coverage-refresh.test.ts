// The four flags, driven directly.
//
// This suite exists because a reviewer had to transcribe `refreshCoverage` into a node script to
// find the defect in the fix that added the fourth flag: setting `stale` for "a refresh was asked
// for while one was out" is swallowed by the very read it waits for. Nothing in the browser suite
// can produce two overlapping reads with a `CellDone` between them, so all four flags shipped with
// no test at all and the defect was invisible to 448 unit tests and 69 browser tests.
import { describe, expect, it, vi } from 'vitest';

import { createCoverageRefresh } from './coverage-refresh';

/**
 * A core whose reads are held open until a test lets them answer, so two can overlap.
 * Every read is recorded, which is what the storm cases count.
 */
function heldReads() {
  const pending: Array<(answer: number | null) => void> = [];
  const rejects: Array<(cause: unknown) => void> = [];
  let asks = 0;
  return {
    get asks() { return asks; },
    pending,
    read: () => {
      asks += 1;
      return new Promise<number | null>((resolve, reject) => {
        pending.push(resolve);
        rejects.push(reject);
      });
    },
    /** Fails the oldest outstanding read, the way a port that throws would. */
    reject: (cause: unknown) => {
      const fail = rejects.shift();
      if (fail === undefined) throw new Error('nothing was asked');
      fail(cause);
    },
    /** Answers the oldest outstanding read. */
    answer: async (value: number | null) => {
      const resolve = pending.shift();
      if (resolve === undefined) throw new Error('nothing was asked');
      resolve(value);
      // Two turns: one for the awaiting `refresh`, one for whatever it starts.
      await Promise.resolve();
      await Promise.resolve();
    },
  };
}

function refresher(core: ReturnType<typeof heldReads>, accepted: number[], options: {
  stopped?: () => boolean;
  enabled?: () => boolean;
} = {}) {
  return createCoverageRefresh<number>({
    enabled: options.enabled ?? (() => true),
    read: core.read,
    accept: (state) => { accepted.push(state); },
    stopped: options.stopped ?? (() => false),
    now: () => Date.now(),
    later: (run, ms) => { setTimeout(run, ms); },
    retryMs: 1000,
  });
}

describe('the coverage refresh', () => {
  it('issues the refresh that was asked for while a read was out', async () => {
    // The defect the fourth flag was written for, and then reintroduced by it. A `CellDone` lands
    // while a read issued on an earlier tick is still open. The worker answers in order, so that
    // open read was computed *before* the cell was banked: its answer is the old count, and it is
    // the last one anybody asks for unless the deferral survives it.
    //
    // Setting `stale` here instead fails exactly here — the answer below clears `stale` on its way
    // past, and `asks` stays at 1 for ever.
    const core = heldReads();
    const accepted: number[] = [];
    const coverage = refresher(core, accepted);

    void coverage.refresh();
    expect(core.asks).toBe(1);

    // The `CellDone`, arriving while that one is still open.
    void coverage.refresh();
    expect(core.asks, 'a second read went out on top of the first').toBe(1);

    // The stale answer lands.
    await core.answer(0);
    expect(accepted).toEqual([0]);
    expect(core.asks, 'the deferred refresh was dropped').toBe(2);

    await core.answer(1);
    expect(accepted, 'the bank was never read back').toEqual([0, 1]);
  });

  it('defers at most one refresh per completed read', async () => {
    // The bound on the flag: it is cleared before the follow-up goes out, so three asks during one
    // read produce one follow-up, not three. Without that, a burst of `CellDone`s would queue a
    // read each and the throttle would not see any of them.
    const core = heldReads();
    const accepted: number[] = [];
    const coverage = refresher(core, accepted);

    void coverage.refresh();
    void coverage.refresh();
    void coverage.refresh();
    void coverage.refresh();
    expect(core.asks).toBe(1);

    await core.answer(0);
    expect(core.asks).toBe(2);

    await core.answer(1);
    expect(core.asks, 'a deferral outlived the read it was deferred behind').toBe(2);
  });

  it('does not put a second read on top of a slow refusal', async () => {
    // The storm the in-flight flag ended: the throttle is armed when the answer comes back, so a
    // refusal that takes longer than the interval leaves every subsequent frame free to ask.
    vi.useFakeTimers();
    try {
      const core = heldReads();
      const accepted: number[] = [];
      const coverage = refresher(core, accepted);

      // A refusal first, because `isDue()` reads `stale` and only an *answered* refusal sets it.
      // Without this the loop below never calls `refresh` at all — a reviewer instrumented the
      // first version of this test and measured 0 retries out of 300 frames, so it asserted that
      // one read stays one read while nothing was ever in a position to make a second.
      void coverage.refresh();
      await core.answer(null);
      expect(core.asks).toBe(1);

      // Now the retry goes out and is slow. `stale` is still set — only an answer clears it — so
      // every frame past the next interval boundary asks again, and the in-flight guard is the
      // only thing standing there. Five seconds of animation frames crosses four of them.
      for (let frame = 0; frame < 300; frame += 1) {
        vi.advanceTimersByTime(16);
        if (coverage.isDue()) void coverage.refresh();
      }
      expect(core.asks, 'a slow read was asked again while it was still out').toBe(2);
    } finally {
      vi.useRealTimers();
    }
  });

  it('retries a refused read, but not before the interval', async () => {
    // A refused read is the one that makes a captured cell permanently invisible, so it is retried
    // — and the interval is what stops that retry becoming a facade round trip per frame.
    vi.useFakeTimers();
    try {
      const core = heldReads();
      const accepted: number[] = [];
      const coverage = refresher(core, accepted);

      void coverage.refresh();
      await core.answer(null);
      expect(accepted, 'a refusal was recorded as an answer').toEqual([]);
      expect(coverage.isDue(), 'the retry fired inside its own interval').toBe(false);

      vi.advanceTimersByTime(1000);
      expect(coverage.isDue()).toBe(true);
      void coverage.refresh();
      expect(core.asks).toBe(2);

      // And an answer ends it: nothing is owed once the map is right.
      await core.answer(7);
      vi.advanceTimersByTime(5000);
      expect(coverage.isDue(), 'an answered read left the retry armed').toBe(false);
    } finally {
      vi.useRealTimers();
    }
  });

  it('measures the interval from the ask, not from the answer', async () => {
    // The clock is armed when the call goes out, and a reviewer sabotaged the other spelling to
    // check: with it armed on the answer, a read that takes most of the interval to refuse pushes
    // the retry a whole further interval away, and a slow core is the one that most needs the
    // retry to be on time. Invisible unless the read actually takes time, which is why every other
    // case here resolves instantly and this one does not.
    vi.useFakeTimers();
    try {
      const core = heldReads();
      const accepted: number[] = [];
      const coverage = refresher(core, accepted);

      void coverage.refresh();
      vi.advanceTimersByTime(900);
      await core.answer(null);
      expect(coverage.isDue(), 'the retry fired inside its own interval').toBe(false);

      vi.advanceTimersByTime(100);
      expect(coverage.isDue(), 'the interval was measured from the answer rather than the ask')
        .toBe(true);
    } finally {
      vi.useRealTimers();
    }
  });

  it('gives a stopped loop one late retry and only one', async () => {
    // The window: a read in flight across the camera going away comes back refused, and the render
    // loop that would have retried it has stopped. Latched, because if the core is what died the
    // second attempt fails the same way and asking for ever is worse than a stale dot.
    vi.useFakeTimers();
    try {
      const core = heldReads();
      const accepted: number[] = [];
      const coverage = refresher(core, accepted, { stopped: () => true });

      void coverage.refresh();
      await core.answer(null);
      expect(core.asks).toBe(1);

      await vi.advanceTimersByTimeAsync(1000);
      expect(core.asks, 'the stopped loop got no retry at all').toBe(2);

      await core.answer(null);
      await vi.advanceTimersByTimeAsync(10000);
      expect(core.asks, 'the one retry was not latched').toBe(2);
    } finally {
      vi.useRealTimers();
    }
  });

  it('does not let a deferred ask spend a second retry on a stopped loop', async () => {
    // The latch says "one retry", and a deferred ask made it two. If something asked while the
    // read that then *failed* was out, `askedAgain` survived the refusal — so the latched retry
    // answered, and its success then issued the deferred read as a third ask. A reviewer measured
    // three where this asserts two.
    //
    // The refusal path is where the deferred ask is *subsumed* rather than owed: the retry it
    // schedules is the re-ask, and when the loop is still running `stale` is what re-asks. Either
    // way somebody is going to look again, so carrying the flag past a refusal double-counts it.
    vi.useFakeTimers();
    try {
      const core = heldReads();
      const accepted: number[] = [];
      const coverage = refresher(core, accepted, { stopped: () => true });

      void coverage.refresh();
      // The `CellDone` that arrives while the first read is still out.
      void coverage.refresh();
      expect(core.asks).toBe(1);

      await core.answer(null);          // the first read refuses
      await vi.advanceTimersByTimeAsync(1000);
      expect(core.asks, 'the latched retry did not go out').toBe(2);

      await core.answer(7);             // the retry answers
      await vi.advanceTimersByTimeAsync(10000);
      expect(core.asks, 'a deferred ask spent a second retry past the latch').toBe(2);
      expect(accepted).toEqual([7]);
    } finally {
      vi.useRealTimers();
    }
  });

  it('recovers when a read rejects rather than answering', async () => {
    // `read`'s contract is "`null` is a refusal, whatever produced it", and the composition root
    // honours that with a `.catch`. But a port that rejects instead would leave `inFlight` true
    // for ever: every later refresh takes the deferral branch, `isDue()` is false because `stale`
    // was never set, and all four flags are wedged for the session with nothing able to recover.
    //
    // A rejection is not this module's to interpret, but it is this module's to survive.
    const core = heldReads();
    const accepted: number[] = [];
    const coverage = refresher(core, accepted);

    const first = coverage.refresh();
    core.reject(new Error('the worker went away'));
    await expect(first).rejects.toThrow('the worker went away');

    // The next ask must actually go out rather than being deferred behind a read that ended.
    void coverage.refresh();
    expect(core.asks, 'a rejected read wedged the in-flight flag').toBe(2);
  });

  it('asks nothing when there is no plan to ask about', async () => {
    const core = heldReads();
    const accepted: number[] = [];
    const coverage = refresher(core, accepted, { enabled: () => false });

    await coverage.refresh();
    expect(core.asks).toBe(0);
  });
});
