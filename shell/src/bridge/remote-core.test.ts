// The page's end of the worker boundary (ADR 0019).
//
// What is worth testing here is the part that is hand-written rather than generated: requests are
// paired with their answers by a number, and a protocol that gets that wrong returns one call's
// result to another caller — which looks like a core that is subtly wrong rather than a page that
// mixed up two replies.
import { describe, expect, it, vi } from 'vitest';

import { connectCore, type WorkerLike } from './remote-core';
import type { FromWorker, ToWorker } from './protocol';

/** A worker that records what it was sent and replies only when the test says so. */
function fakeWorker() {
  const sent: { message: ToWorker; transfer?: Transferable[] }[] = [];
  // Kept per type rather than in one slot. A single variable was enough while `message` was the
  // only event listened for, and would now silently drop it: the error handlers register after it
  // and the last one would win, so the tests that matter would pass against a page that never
  // heard a reply.
  const listeners = new Map<string, (event: never) => void>();
  let terminated = false;

  const worker: WorkerLike = {
    postMessage(message, transfer) { sent.push({ message, transfer }); },
    addEventListener(type: string, handler: (event: never) => void) { listeners.set(type, handler); },
    terminate() { terminated = true; },
  } as WorkerLike;

  const reply = (message: FromWorker) =>
    (listeners.get('message') as ((e: MessageEvent<FromWorker>) => void) | undefined)?.(
      { data: message } as MessageEvent<FromWorker>);
  const raise = (type: 'error' | 'messageerror', event?: unknown) =>
    (listeners.get(type) as ((e: unknown) => void) | undefined)?.(event);
  const seqOf = (kind: ToWorker['kind']) => {
    const found = sent.find((entry) => entry.message.kind === kind);
    return found && 'seq' in found.message ? found.message.seq : -1;
  };
  // Boot is answered eagerly, since connectCore cannot return until it is.
  const boot = async (methods: string[] = ['ProjectManager.list']) => {
    const connecting = connectCore(worker, 'https://example.test/core.js');
    await Promise.resolve();
    reply({ kind: 'booted', seq: seqOf('boot'), methods, spill: true });
    return connecting;
  };
  return { worker, sent, reply, raise, seqOf, boot, wasTerminated: () => terminated };
}

describe('connecting', () => {
  it('passes the core URL across, because only the page knows the base path', async () => {
    const w = fakeWorker();
    await w.boot();
    expect(w.sent[0].message).toMatchObject({ kind: 'boot', coreUrl: 'https://example.test/core.js' });
  });

  it('reports the methods the worker published rather than a list of its own', async () => {
    const w = fakeWorker();
    const { core } = await w.boot(['ProjectManager.list', 'CaptureSessionManager.armBurst']);
    expect(core.methods()).toEqual(['ProjectManager.list', 'CaptureSessionManager.armBurst']);
  });
});

describe('pairing answers with the calls that asked for them', () => {
  it('does not hand one call the answer to another', async () => {
    // The failure this prevents is not a crash. Two calls in flight and a protocol that pairs
    // them by arrival order returns B's bytes to A, which decodes into a plausible wrong value
    // and surfaces as the core being wrong somewhere else entirely.
    const w = fakeWorker();
    const { core } = await w.boot();

    const first = core.call('ProjectManager.list', new Uint8Array([1]));
    const second = core.call('ProjectManager.list', new Uint8Array([2]));
    const calls = w.sent.filter((entry) => entry.message.kind === 'call');
    expect(calls).toHaveLength(2);

    const seqA = (calls[0].message as Extract<ToWorker, { kind: 'call' }>).seq;
    const seqB = (calls[1].message as Extract<ToWorker, { kind: 'call' }>).seq;
    expect(seqA).not.toBe(seqB);

    // Answered out of order, which is the whole point: the worker is free to.
    w.reply({ kind: 'result', seq: seqB, bytes: new Uint8Array([0xbb]) });
    w.reply({ kind: 'result', seq: seqA, bytes: new Uint8Array([0xaa]) });

    expect(Array.from(await first)).toEqual([0xaa]);
    expect(Array.from(await second)).toEqual([0xbb]);
  });

  it('rejects with the reason the worker gave, not a generic one', async () => {
    // Startup is three failures now — the worker starts, the module loads, the store opens — so
    // the detail is what tells the client which, and replacing it would lose that.
    const w = fakeWorker();
    const { core } = await w.boot();
    const call = core.call('ProjectManager.list', new Uint8Array());
    const seq = (w.sent.find((e) => e.message.kind === 'call')!.message as { seq: number }).seq;

    w.reply({ kind: 'failed', seq, detail: 'the core is not loaded' });
    await expect(call).rejects.toThrow('the core is not loaded');
  });

  it('ignores an answer nobody is waiting for instead of throwing', async () => {
    const w = fakeWorker();
    await w.boot();
    expect(() => w.reply({ kind: 'result', seq: 9999, bytes: new Uint8Array() })).not.toThrow();
  });
});

describe('the pushes', () => {
  it('hands the IMU buffer over by transfer rather than copying it', async () => {
    // A batch crosses at sensor rate. Transferred it costs the same at any length; cloned it
    // would pay per sample, which is the thing this boundary cannot afford to do per frame.
    const w = fakeWorker();
    const { remote } = await w.boot();
    const doubles = new Float64Array([1, 2, 3]);

    remote.pushMotion(doubles);
    const push = w.sent.find((entry) => entry.message.kind === 'imu')!;
    expect(push.transfer).toEqual([doubles.buffer]);
  });

  it('sends a cleared camera rather than going quiet, so the core cannot plan against a lens that is gone', async () => {
    const w = fakeWorker();
    const { remote } = await w.boot();
    remote.setCamera(null);
    expect(w.sent.find((entry) => entry.message.kind === 'camera')!.message)
      .toMatchObject({ kind: 'camera', opened: null });
  });
});

describe('what the worker asks back', () => {
  it('runs the close handler, because only this side can stop a MediaStream', async () => {
    const w = fakeWorker();
    const { remote } = await w.boot();
    const stop = vi.fn();
    remote.onCloseCamera(stop);

    w.reply({ kind: 'closeCamera' });
    expect(stop).toHaveBeenCalledTimes(1);
  });

  it('carries a persist failure back from the flush that found it', async () => {
    const w = fakeWorker();
    const { remote } = await w.boot();
    const flushing = remote.flush();
    const seq = (w.sent.find((e) => e.message.kind === 'flush')!.message as { seq: number }).seq;

    w.reply({ kind: 'flushed', seq, persistError: 'quota exceeded' });
    await expect(flushing).resolves.toBe('quota exceeded');
  });
});

describe('a worker that stops answering', () => {
  it('fails the calls in flight instead of leaving them pending for ever', async () => {
    // The failure this exists for is the quietest one: a worker script that 404s or throws at the
    // top level never replies, and `main()` awaits the boot — so the page sat on its loading
    // screen with nothing shown, because the code that writes the failure UI is downstream of the
    // await that never returned.
    const w = fakeWorker();
    const core = await w.boot();
    const inFlight = core.remote.flush();

    w.raise('error', { message: 'Failed to fetch worker.js' });

    await expect(inFlight).rejects.toThrow(/Failed to fetch worker\.js/);
  });

  it('fails a call made after the failure rather than queueing it', async () => {
    const w = fakeWorker();
    const core = await w.boot();
    w.raise('error', { message: 'boom' });

    await expect(core.remote.flush()).rejects.toThrow(/boom/);
  });

  it('gives up the thread, so a page reporting the failure is not still holding one', async () => {
    const w = fakeWorker();
    await w.boot();
    w.raise('error', { message: 'boom' });

    expect(w.wasTerminated()).toBe(true);
  });

  it('treats an undeserialisable message as the same kind of death', async () => {
    // `messageerror` means a reply arrived and could not be read. It carries no sequence number,
    // so there is no call to fail in particular — and the one that was waiting for it never hears
    // anything otherwise.
    const w = fakeWorker();
    const core = await w.boot();
    const inFlight = core.remote.flush();

    w.raise('messageerror');

    await expect(inFlight).rejects.toThrow(/deserialise/);
  });

  it('fails a boot the worker never answers, once it is known to be dead', async () => {
    const w = fakeWorker();
    const connecting = connectCore(w.worker, 'https://example.test/core.js');
    await Promise.resolve();

    w.raise('error', { message: 'the script would not load' });

    await expect(connecting).rejects.toThrow(/the script would not load/);
  });
});
