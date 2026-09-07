// The queue and the clock, tested where they can actually fail.
//
// This suite exists because a reviewer showed the browser test named for the clock — `a slow
// camera spends its lock budget on the track, not in the queue` — passing with the enqueue-clock
// defect reintroduced, at 400 ms per constraint and at 700 ms. Nothing in that arrangement ever
// puts two writes on the chain at once, so the two clocks are indistinguishable through the page.
// Contention is one line here.
import { describe, expect, it, vi } from 'vitest';

import { createLockWriteChain } from './lock-writes';

/** A camera that takes a stated time to answer, and records the order it was asked in. */
function slowCamera(ms: number) {
  const asked: string[] = [];
  const answered: string[] = [];
  return {
    asked,
    answered,
    write: (wanted: string) => {
      asked.push(wanted);
      return new Promise<string>((resolve) => {
        setTimeout(() => { answered.push(wanted); resolve(`${wanted} ok`); }, ms);
      });
    },
  };
}

describe('the lock write chain', () => {
  it('spends a write budget on the track, not in the queue', async () => {
    // The defect this is named for, in the one arrangement that separates the two clocks: three
    // writes, each taking two thirds of the budget. Every one reaches the track well inside its
    // own three seconds; the third does not join the queue until four seconds in.
    //
    // With the clock started at enqueue the third is abandoned — and in the page that abandonment
    // queues a release behind the write it gave up on, so the queue grows by one write per
    // failure and every burst after the first is refused.
    vi.useFakeTimers();
    try {
      const camera = slowCamera(2000);
      const writeLocks = createLockWriteChain(camera.write, 3000);

      const first = writeLocks('one');
      const second = writeLocks('two');
      const third = writeLocks('three');

      await vi.advanceTimersByTimeAsync(6000);

      expect(await first).toEqual({ answered: true, done: 'one ok' });
      expect(await second).toEqual({ answered: true, done: 'two ok' });
      expect(await third, 'the third write spent its budget waiting for the first two')
        .toEqual({ answered: true, done: 'three ok' });
      expect(camera.answered, 'the writes reached the track out of order').toEqual(
        ['one', 'two', 'three']);
    } finally {
      vi.useRealTimers();
    }
  });

  it('gives up on a write the track never answers, and says it did not answer', async () => {
    // "No answer" is not "refused". A caller told the second would fire a burst believing the
    // camera had declined; a caller told the first knows nothing about the camera's state, which
    // is the only honest thing to say and the reason `answered` is a separate field (ADR 0022).
    vi.useFakeTimers();
    try {
      const writeLocks = createLockWriteChain(() => new Promise<string>(() => {}), 3000);
      const stuck = writeLocks('one');

      await vi.advanceTimersByTimeAsync(2999);
      let settled = false;
      void stuck.then(() => { settled = true; });
      await Promise.resolve();
      expect(settled, 'gave up before the budget was spent').toBe(false);

      await vi.advanceTimersByTimeAsync(2);
      expect(await stuck).toEqual({ answered: false });
    } finally {
      vi.useRealTimers();
    }
  });

  it('does not let the queue advance past a write nobody is waiting for any more', async () => {
    // The half that is easy to get backwards. The caller stops waiting; the *chain* does not — if
    // it advanced on the timeout, the release queued behind a stuck write would reach the track
    // first and the stuck one would land afterwards, ending a session with the camera locked.
    // Which is the exact failure the ordering exists to prevent.
    vi.useFakeTimers();
    try {
      const asked: string[] = [];
      let releaseStuck: (value: string) => void = () => {};
      const writeLocks = createLockWriteChain((wanted: string) => {
        asked.push(wanted);
        return wanted === 'stuck'
          ? new Promise<string>((resolve) => { releaseStuck = resolve; })
          : Promise.resolve(`${wanted} ok`);
      }, 3000);

      const stuck = writeLocks('stuck');
      const behind = writeLocks('release');

      await vi.advanceTimersByTimeAsync(5000);
      expect(await stuck).toEqual({ answered: false });
      expect(asked, 'the release overtook the write it was queued behind').toEqual(['stuck']);

      releaseStuck('stuck ok');
      await vi.advanceTimersByTimeAsync(1);
      expect(await behind).toEqual({ answered: true, done: 'release ok' });
      expect(asked).toEqual(['stuck', 'release']);
    } finally {
      vi.useRealTimers();
    }
  });

  it('carries on after a write the camera rejected', async () => {
    // A camera that refuses a constraint is a supported outcome, and the release after it is
    // exactly when the ordering matters most — so a rejection must not park the chain.
    const asked: string[] = [];
    const writeLocks = createLockWriteChain((wanted: string) => {
      asked.push(wanted);
      return wanted === 'bad' ? Promise.reject(new Error('no')) : Promise.resolve(`${wanted} ok`);
    }, 3000);

    const refused = writeLocks('bad').catch(() => 'threw');
    const after = writeLocks('good');

    expect(await refused).toBe('threw');
    expect(await after).toEqual({ answered: true, done: 'good ok' });
    expect(asked).toEqual(['bad', 'good']);
  });
});
