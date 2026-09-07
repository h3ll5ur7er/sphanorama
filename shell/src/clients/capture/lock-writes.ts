/**
 * The queue every camera lock write goes through, and the clock that bounds one.
 *
 * Three callers write the camera's locks — an arm, the release after a refused arm, and the core's
 * own `onReleaseLocks` at the end of a burst — and none of them awaits the others. A release
 * issued for burst 1 could land *after* burst 2 had read the state back and told the core three
 * locks were held; measured landing 80 ms into a burst the core believed was locked. ADR 0022's
 * read-back cannot catch that, because the write it would have to see happens after the read.
 *
 * A chain rather than a lock: the writes are all short, order is the only thing that matters, and
 * a failed one must not stop the next — a camera that refuses a constraint is a supported outcome,
 * and the release after it is exactly when the ordering matters most.
 *
 * It lives here rather than in `main.ts` because the invariant is arithmetic about a queue and a
 * clock, and in `main.ts` the only way to reach it was through a browser. A reviewer showed what
 * that cost: the browser test named for the clock could not fail against the defect in its own
 * header, because nothing in the arrangement ever contended the chain. Here it takes four lines.
 */

/**
 * What a caller gets back: the camera's answer, or the fact that there was not one.
 *
 * The two used to be the same value. A write that timed out was manufactured into a refusal — the
 * same shape a camera that *says no* produces — and every caller downstream then treated "the
 * track has not answered" as "the track refused". They are not the same fact and the difference is
 * the whole of ADR 0022: a refusal is a known state of the camera, and a timeout is no state at
 * all. Firing a burst over the second is firing over an exposure that may change halfway through
 * it, which is exactly what locks exist to prevent.
 */
export type LockWrite<T> = { answered: true; done: T } | { answered: false };

/**
 * How long a single write may take before the caller gives up waiting for it.
 *
 * `applyConstraints` is a promise the platform owns, and a camera that never settles one is not
 * hypothetical — a track pulled away mid-call resolves nothing. Without a bound the chain parks
 * for the life of the tab: every later write queues behind it, and because arming awaits its own
 * write, no burst can ever be armed again. A slow camera merely arrives late; this is what stops a
 * stuck one from being permanent.
 */
export const LOCK_WRITE_TIMEOUT_MS = 3000;

export function createLockWriteChain<W, T>(
  write: (wanted: W) => Promise<T>,
  timeoutMs: number = LOCK_WRITE_TIMEOUT_MS,
): (wanted: W) => Promise<LockWrite<T>> {
  let chain: Promise<unknown> = Promise.resolve();

  return (wanted: W): Promise<LockWrite<T>> => {
    // The *caller* stops waiting after the timeout; the chain does not. Those are different things
    // and conflating them is worse than having no timeout at all: if the queue advanced on the
    // race, a stuck write would be overtaken by the release queued behind it, the release would
    // reach the track first, and the stuck one would land afterwards — ending a session with the
    // camera locked, which is the failure the ordering exists to prevent.
    //
    // So the chain is built from the real promise and only the answer is raced. A stuck write
    // still delays everything behind it, and that is correct: a camera that has not answered has
    // not answered, and guessing the order it will finish in is what produced the bug above.
    //
    // The clock starts when this write reaches the track, not when it joins the queue. Started at
    // enqueue, the budget was spent waiting for the writes in front — and each refusal queued a
    // release of its own behind the write it had given up on, so the queue grew by one write per
    // failure. Measured on a camera taking 700 ms per constraint: the first burst succeeded, the
    // second was refused after 3007 ms of which 2463 were spent in the queue, and every burst
    // after it failed the same way at 3 s intervals while the track answered fourteen constraints
    // back to back without an idle moment. One cell per session, blamed on a camera that was
    // answering everything it was asked.
    let reached: () => void;
    const reaches = new Promise<void>((resolve) => { reached = resolve; });
    const settled = chain.then(() => {
      reached();
      return write(wanted);
    });
    chain = settled.catch(() => undefined);
    return Promise.race([
      settled.then((done): LockWrite<T> => ({ answered: true, done })),
      reaches.then(() => new Promise<LockWrite<T>>((resolve) => {
        setTimeout(() => resolve({ answered: false }), timeoutMs);
      })),
    ]);
  };
}
