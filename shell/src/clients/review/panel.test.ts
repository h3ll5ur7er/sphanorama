// The panel's own decisions, which are about *when* an answer is allowed to reach the DOM. What
// the strip says is candidateStrip's business and what the map looks like is reviewed by eye;
// what is here is the ordering the panel has to survive because every call it makes crosses a
// worker.
import { describe, expect, it } from 'vitest';

import {
  createReviewPanel, paintPreviewOnCanvas, previewIsDrawable, PREVIEW_MAX_EDGE,
  type PaintPreview, type ReviewCore, type ReviewElements,
} from './panel';
import type {
  Candidate, CandidateId, CapturePlan, CoverageState, FramePreview, NodeId,
} from '../../../../contracts/ts/contracts';

/**
 * Detached elements rather than a shared document: two panels in one file would otherwise both
 * answer to `#strip`, and a test would pass on another test's DOM.
 */
function elements(): ReviewElements {
  const make = () => document.createElement('div');
  return { panel: make(), map: make(), stripHeading: make(), strip: make() };
}

function candidate(id: number, node: number): Candidate {
  return {
    id, node, frame: { id },
    quality: { sharpness: id / 10, exposureAgreement: 1, motionBlur: 0, aggregate: id / 10 },
  } as unknown as Candidate;
}

/**
 * Enough microtask turns for the panel's awaits to unwind.
 *
 * Counting individual `await Promise.resolve()`s is how these tests used to wait, and it broke the
 * day `open()` gained a second call to await alongside the first: the panel had not changed what
 * it does, only how many ticks it takes to get there. A settle is indifferent to that.
 */
async function settle(turns = 6): Promise<void> {
  for (let turn = 0; turn < turns; turn += 1) await Promise.resolve();
}

/** The shape the facade answers with, mirrored here so the fake core can hand one back. */
type Answered<T> = { readonly ok: true; readonly value: T } | { readonly ok: false };

/** A preview of a candidate, sized as the core would size one. */
function preview(candidate: number): FramePreview {
  const width = 4;
  const height = 3;
  return {
    frame: candidate as unknown as FramePreview['frame'],
    width,
    height,
    format: 'RGBA8',
    pixels: new Uint8Array(width * height * 4).fill(candidate),
  };
}

/** A painter that records what it was asked to draw, so the DOM never needs a real canvas. */
function recordingPainter() {
  const painted: Array<{ candidate: number; canvas: HTMLCanvasElement }> = [];
  const paint: PaintPreview = (canvas, drawn) => {
    canvas.dataset.preview = 'ready';
    painted.push({ candidate: drawn.frame as unknown as number, canvas });
  };
  return { paint, painted };
}

/** A core whose answers are released by hand, so an out-of-order reply can be staged. */
function deferredCore() {
  const pending = new Map<number, (candidates: Candidate[]) => void>();
  // An answer staged before the call that will collect it. The panel re-opens a cell from inside
  // a click handler, so a test cannot always be holding the promise at the moment it wants to
  // decide what comes back.
  const queued = new Map<number, Candidate[]>();
  const previewsPending = new Map<number, (answer: Answered<FramePreview>) => void>();
  const previewCalls: Array<{ node: number; candidate: number; maxEdge: number }> = [];
  // Which candidates have no frame any more. A replace-retake forgets a cell's frames while a
  // strip fetched a moment ago is still naming them.
  const gone = new Set<number>();
  // Whether previews answer as they are asked for, or wait to be released by hand.
  let holdPreviews = false;
  // The selections the core is holding — the thing the panel used to keep for itself.
  const recorded = new Map<number, number>();
  let writesLand = true;
  let selectionReadable = true;
  const writes: Array<() => void> = [];
  // How many times the panel asked for a cell's list. A refresh that never happened is invisible
  // in the DOM when the core's answer has not changed, so the count is what tells the two apart.
  const candidateCalls: number[] = [];
  // A core whose calls reject rather than answer, as `remote-core`'s `die()` makes every call do
  // once the worker is gone.
  let reads: 'answer' | 'reject' = 'answer';
  const core: ReviewCore = {
    candidates: (node: NodeId) => new Promise((resolve, reject) => {
      candidateCalls.push(node as number);
      if (reads === 'reject') { reject(new Error('the worker is gone')); return; }
      const staged = queued.get(node as number);
      if (staged !== undefined) {
        queued.delete(node as number);
        resolve({ ok: true, value: staged });
        return;
      }
      pending.set(node as number, (value) => resolve({ ok: true, value }));
    }),
    candidatePreview: (node: NodeId, candidate: CandidateId, maxEdge: number) =>
      new Promise((resolve, reject) => {
        previewCalls.push({
          node: node as number, candidate: candidate as number, maxEdge,
        });
        if (reads === 'reject') { reject(new Error('the worker is gone')); return; }
        const answer: Answered<FramePreview> = gone.has(candidate as number)
          ? { ok: false }
          : { ok: true, value: preview(candidate as number) };
        if (holdPreviews) previewsPending.set(candidate as number, resolve);
        else resolve(answer);
      }),
    setSelection: (node: NodeId, candidate: CandidateId) => new Promise((resolve) => {
      writes.push(() => {
        // The core is what remembers, which is the whole point of the change: the panel keeps no
        // copy, so a write that never lands leaves the previous answer standing.
        if (writesLand) recorded.set(node as number, candidate as number);
        resolve({ ok: writesLand });
      });
    }),
    selection: (node: NodeId) => (reads === 'reject'
      ? Promise.reject(new Error('the worker is gone'))
      : Promise.resolve(
        selectionReadable
          ? { ok: true as const, value: (recorded.get(node as number) ?? 0) as CandidateId }
          : { ok: false as const })),
  };
  async function releaseWrite(index: number): Promise<void> {
    const [release] = writes.splice(index, 1);
    if (release === undefined) throw new Error(`no selection is in flight at ${index}`);
    release();
    // Enough turns for the click handler to resume and for the open() it may then await to run
    // its own continuation.
    for (let turn = 0; turn < 4; turn += 1) await Promise.resolve();
  }

  return {
    core,
    previewCalls,
    candidateCalls,
    /** Make every call reject, as a dead worker does. */
    killWorker() { reads = 'reject'; },
    /** What the core already has recorded, as a reload would find it. */
    record(node: number, candidate: number) { recorded.set(node, candidate); },
    /** Make the next writes fail, as a core that refuses would. */
    refuseWrites() { writesLand = false; },
    /** Make reading a selection fail, which the strip has to survive. */
    refuseReads() { selectionReadable = false; },
    forget(candidate: number) { gone.add(candidate); },
    holdPreviews() { holdPreviews = true; },
    releasePreview(candidate: number) {
      const release = previewsPending.get(candidate);
      if (release === undefined) throw new Error(`no preview in flight for ${candidate}`);
      previewsPending.delete(candidate);
      release(gone.has(candidate) ? { ok: false } : { ok: true, value: preview(candidate) });
    },
    answer(node: number, candidates: Candidate[]) {
      const release = pending.get(node);
      if (release === undefined) { queued.set(node, candidates); return; }
      pending.delete(node);
      release(candidates);
    },
    /** How many writes are waiting to be let through. */
    writesInFlight() { return writes.length; },
    /** Lets the oldest in-flight SetSelection through, and waits for what it set off to settle. */
    async recorded() { await releaseWrite(0); },
    /**
     * Lets one in-flight SetSelection through by position, oldest first.
     *
     * Releasing out of order is the whole point of having this: the worker happens to answer in
     * the order it was asked today, and the panel deliberately does not assume so. A fake that
     * could only ever release the oldest would agree with the panel's comments while quietly
     * testing the opposite.
     */
    recordedAt: releaseWrite,
  };
}

/** A node aimed at an azimuth, built the way the core's FromAzimuthElevation builds one. */
function node(id: number, azimuthDeg: number) {
  const half = (azimuthDeg * Math.PI) / 360;
  return {
    id, acceptanceConeDeg: 4, ringIndex: 0,
    targetOrientation: { w: Math.cos(half), x: 0, y: Math.sin(half), z: 0 },
  };
}

const plan = { nodes: [node(1, 0), node(2, 90)], spec: {} } as unknown as CapturePlan;
const coverage = { nodesSatisfied: 0, holes: [], underOverlapped: [] } as unknown as CoverageState;

describe('opening a cell', () => {
  it('lets the answer you are waiting for win, whatever order the replies arrive in', async () => {
    // Every call crosses a postMessage to the worker the core runs in, so two taps in quick
    // succession are two answers in flight with no promise that they come back in order. Writing
    // the DOM from whichever settles last puts one cell's candidates under another cell's
    // heading — and the map's pressed dot, which is set synchronously, would name the second.
    const { core, answer } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);

    const first = panel.open(1 as NodeId);
    const second = panel.open(2 as NodeId);
    answer(2, [candidate(20, 2)]);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await Promise.all([first, second]);

    expect(Array.from(ui.strip.querySelectorAll('button'), (b) => b.textContent)).toEqual([
      expect.stringContaining('#20'),
    ]);
  });

  it('does not haul you back to the cell you were on when a pick is recorded late', async () => {
    // Recording a pick re-opens the cell so the strip can show what is now in force. But
    // SetSelection crosses the worker too, so the user can have moved on by the time it answers —
    // and a re-open then wins on both halves at once, because it takes the newest ticket *and*
    // sets `opened`, which is what the map's pressed dot is drawn from.
    const { core, answer, recorded } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);

    const first = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1)]);
    await first;
    ui.strip.querySelector('button')!.click();

    const second = panel.open(2 as NodeId);
    answer(2, [candidate(20, 2)]);
    await second;
    await recorded();

    expect(Array.from(ui.strip.querySelectorAll('button'), (b) => b.textContent)).toEqual([
      expect.stringContaining('#20'),
    ]);
    panel.show(plan, coverage);
    const pressed = Array.from(ui.map.querySelectorAll('.cell'))
      .filter((dot) => dot.getAttribute('aria-pressed') === 'true')
      .map((dot) => dot.getAttribute('aria-label'));
    expect(pressed).toEqual([expect.stringContaining('Cell 2')]);
  });

  it('does refresh the strip when the pick lands and you are still on that cell', async () => {
    // The other half of the guard above: staying put has to still redraw, or the strip would go
    // on showing the core's automatic pick as the one in force after you overrode it.
    const { core, answer, recorded } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);

    const opening = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await opening;
    ui.strip.querySelectorAll('button')[1].click();
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await recorded();

    const pressed = Array.from(ui.strip.querySelectorAll('button'))
      .filter((button) => button.getAttribute('aria-pressed') === 'true')
      .map((button) => button.textContent);
    expect(pressed).toEqual([expect.stringContaining('#11')]);
  });

  it('shows each candidate\'s frame rather than only what the core knows about it', async () => {
    // The gap this closes. A strip of numbers cannot answer the question a burst exists to raise
    // — which of these five frames of the same wall is the one to keep — and the pixels had no
    // way out of the core until `CandidatePreview` (ADR 0038).
    const { core, answer, previewCalls } = deferredCore();
    const { paint, painted } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);

    const opening = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await opening;

    // One picture per row, in the order the core ranked them.
    expect(painted.map((p) => p.candidate)).toEqual([10, 11]);
    expect(ui.strip.querySelectorAll('canvas.thumb')).toHaveLength(2);
    expect(Array.from(ui.strip.querySelectorAll('canvas.thumb'),
                      (c) => (c as HTMLElement).dataset.preview)).toEqual(['ready', 'ready']);
    // Asked for at the size the strip draws, not at whatever the frame happens to be.
    expect(previewCalls.map((call) => call.maxEdge)).toEqual([PREVIEW_MAX_EDGE, PREVIEW_MAX_EDGE]);
    // And the numbers are still there: the picture is beside the scores, not instead of them.
    expect(ui.strip.querySelector('button')!.textContent).toContain('sharpness');
  });

  it('leaves a row readable when its frame has gone', async () => {
    // A replace-retake forgets a cell's frames, and a strip fetched a moment before that is still
    // naming them. `candidates.ts` already copes with a selection that outlives what it names;
    // this is the same staleness one call further down, and one missing picture must not take
    // the rest of the strip with it.
    const { core, answer, forget } = deferredCore();
    const { paint, painted } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);
    forget(10);

    const opening = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await opening;

    expect(painted.map((p) => p.candidate)).toEqual([11]);
    expect(Array.from(ui.strip.querySelectorAll('canvas.thumb'),
                      (c) => (c as HTMLElement).dataset.preview)).toEqual(['missing', 'ready']);
    expect(ui.strip.querySelectorAll('button')).toHaveLength(2);
  });

  it('does not paint a preview into the cell that replaced the one it was asked for', async () => {
    // Every preview is its own crossing, so a cell of eight is eight answers in flight while the
    // user is free to tap another dot. Painting whichever arrives would put one cell's frames
    // under another cell's heading — the same race the candidate list already guards, one call
    // further down and eight times as likely.
    const { core, answer, holdPreviews, releasePreview } = deferredCore();
    const { paint, painted } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);
    holdPreviews();

    const first = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1)]);
    await settle();
    const second = panel.open(2 as NodeId);
    answer(2, [candidate(20, 2)]);
    releasePreview(10);
    await first;
    releasePreview(20);
    await second;

    expect(painted.map((p) => p.candidate)).toEqual([20]);
  });

  it('does not read a cell\'s pixels again to show a pick it just recorded', async () => {
    // Recording a pick re-opens the cell so the strip can show what is in force. Every preview it
    // asked for a second time would fault a spilled frame back into the heap and send it away
    // again — hundreds of milliseconds of file work to redraw pictures the page is still holding.
    const { core, answer, recorded, previewCalls } = deferredCore();
    const { paint, painted } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);

    const opening = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await opening;
    expect(previewCalls).toHaveLength(2);

    ui.strip.querySelectorAll('button')[1].click();
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await recorded();

    expect(previewCalls).toHaveLength(2);
    // Redrawn from what was already held, so the refreshed strip still has its pictures.
    expect(painted.map((p) => p.candidate)).toEqual([10, 11, 10, 11]);
  });

  it('drops the pictures it was holding when another cell is opened', async () => {
    // The cache is for the cell on screen. Keeping every cell's thumbnails would be the review
    // client's own copy of the memory problem the reduction exists to solve.
    const { core, answer, previewCalls } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);

    const first = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1)]);
    await first;
    const second = panel.open(2 as NodeId);
    answer(2, [candidate(20, 2)]);
    await second;
    const again = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1)]);
    await again;

    expect(previewCalls.map((call) => call.candidate)).toEqual([10, 20, 10]);
  });

  it('keeps them when a tap for another cell is superseded before it paints', async () => {
    // The cache is dropped for the cell that is *painted*, not for every cell that is asked for.
    // A dot tapped and thought better of never replaces the strip — the render is cancelled at the
    // ticket check — so throwing away the thumbnails on the way in spends the ~350 ms of spill
    // faulting the cache exists to avoid, to fetch back pictures the page never stopped showing.
    const { core, answer, previewCalls } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);

    const first = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await first;

    // Cell 2 asked for and then abandoned: its answer never arrives, and cell 1 is asked for
    // again before it could have.
    void panel.open(2 as NodeId);
    const again = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await again;

    expect(previewCalls.map((call) => call.candidate)).toEqual([10, 11]);
  });

  it('drops the pictures of candidates a retake replaced', async () => {
    // A retake gives a cell new candidates, and the old identities never come back. Clearing only
    // when *another* cell is opened leaves those thumbnails held for as long as the user stays on
    // this one — 48 KB each, growing with every retake, in the client whose whole reason for
    // reducing was to stop holding frames. So the cache is pruned to what the strip is actually
    // showing on every open.
    const { core, answer, previewCalls } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);

    const first = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await first;

    // The same cell, retaken: different identities, none of the old ones.
    const retaken = panel.open(1 as NodeId);
    answer(1, [candidate(12, 1), candidate(13, 1)]);
    await retaken;

    // And back to what it was, which is the only way to ask whether 10 and 11 were still held.
    const restored = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await restored;

    expect(previewCalls.map((call) => call.candidate)).toEqual([10, 11, 12, 13, 10, 11]);
  });

  it('shows the pick the core has recorded, not one it watched being made', async () => {
    // The gap this closes. The panel used to remember its own writes, so what was in force was
    // whatever *this tab* had clicked — and a reload started again from the ranking, silently
    // disagreeing with the build, which reads the document. Now a fresh panel over a core that
    // already has a selection shows it, having watched nothing.
    const { core, answer, record } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    record(1, 11);   // chosen before this panel existed, as a reload would find it

    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);
    const opening = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await opening;

    const pressed = [...ui.strip.querySelectorAll('button')]
      .map((button) => button.getAttribute('aria-pressed'));
    // The second candidate, which is not the ranking's own pick — so this cannot pass by
    // falling back to the first.
    expect(pressed).toEqual(['false', 'true']);
  });

  it('leaves the previous pick in force when the core refuses the new one', async () => {
    // Nothing is remembered on this side, so a refused write needs no undo: re-opening asks the
    // core, and the core still holds what it held. The strip shows what the build will use rather
    // than what the user last touched.
    const { core, answer, record, refuseWrites, recorded, candidateCalls } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    record(1, 11);
    refuseWrites();

    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);
    const opening = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await opening;

    [...ui.strip.querySelectorAll('button')][0].click();
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await recorded();

    const pressed = [...ui.strip.querySelectorAll('button')]
      .map((button) => button.getAttribute('aria-pressed'));
    expect(pressed).toEqual(['false', 'true']);
    // And it is the core that said so, rather than a strip nobody touched. Asserting the state
    // alone would pass just as well against a panel that never refreshed at all, which is the one
    // other way to leave the previous pick standing — and the wrong one, because the next thing
    // it would do with a write the core *took* is show the old answer.
    expect(candidateCalls).toEqual([1, 1]);
  });

  it('says so, and marks nothing in force, when the selection cannot be read', async () => {
    // A read that failed is not a cell nobody has chosen for, and folding the two together is
    // exactly the silence ADR 0040 exists to end: the strip would say the ranking's pick is what
    // the build will use, about a cell whose override could not be read, with no word anywhere.
    // The rows stay — they were read fine, and picking again is how a user repairs the document
    // this failed on — but none of them claims to be in force, and the heading says why.
    const { core, answer, record, refuseReads } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    record(1, 11);
    refuseReads();

    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);
    const opening = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await opening;

    const pressed = [...ui.strip.querySelectorAll('button')]
      .map((button) => button.getAttribute('aria-pressed'));
    expect(pressed).toEqual(['false', 'false']);
    // Exact, because this sentence is the whole of what "says so" means here, and two fragments
    // leave everything between them free — including a sentence that inverts the meaning.
    expect(ui.stripHeading.textContent)
      .toBe('2 candidates, best first. Which one is in force could not be read.');
  });

  it('coalesces two quick picks into one refresh', async () => {
    // Every refresh is two round trips through the worker, and the earlier of two reads a core
    // the later write is about to change — a trip paid to paint something already out of date.
    // Only the newest write refreshes. This is a count rather than an assumption about which
    // answer comes back first, so it holds whichever order they arrive in.
    const { core, answer, recorded, candidateCalls } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);

    const opening = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await opening;

    const buttons = [...ui.strip.querySelectorAll('button')] as HTMLButtonElement[];
    buttons[0].click();
    buttons[1].click();
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await recorded();
    await recorded();

    // The open, and one refresh — not two.
    expect(candidateCalls).toEqual([1, 1]);
    const pressed = [...ui.strip.querySelectorAll('button')]
      .map((button) => button.getAttribute('aria-pressed'));
    expect(pressed).toEqual(['false', 'true']);
  });

  it('says the cell could not be read when the worker is gone, rather than going quiet', async () => {
    // `remote-core`'s `die()` rejects every call in flight and every call after it. A rejection
    // that nobody catches takes `open()` with it: no rows, no heading, no message — the user is
    // told nothing at all, which is worse than being told the wrong thing. Both reads can do it,
    // and `await panel.open(...)` below is what would carry either one out of the panel, so this
    // test bites through its own await rather than through any listener.
    const { core, killWorker } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    killWorker();

    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);
    await panel.open(1 as NodeId);

    expect(ui.stripHeading.textContent).toBe('That cell could not be read.');
    expect(ui.strip.querySelectorAll('button')).toHaveLength(0);
  });

  it('keeps the rows it drew when the worker dies partway through the pictures', async () => {
    // The third way in, and the likeliest: `fillPreviews` is eight sequential round trips where
    // the pair above is one, so a worker that dies mid-strip dies here. Without the same guard the
    // rejection escapes the loop, escapes `open()`, and lands in the `void` behind a dot's click —
    // leaving the rows that were drawn stuck on `pending` with nothing said. The interesting state
    // is the partial one: rows yes, pictures no, panel still coherent.
    const { core, answer, killWorker } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);

    const opening = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    // After the candidate list has been answered and before its pictures are asked for.
    killWorker();
    await opening;

    expect(ui.stripHeading.textContent).toBe('2 candidates, best first.');
    expect(ui.strip.querySelectorAll('button')).toHaveLength(2);
    expect([...ui.strip.querySelectorAll('canvas')].map((c) => c.dataset.preview))
      .toEqual(['missing', 'missing']);
  });

  it('says only that nothing is captured when an empty cell also cannot be read', async () => {
    // No row to mark, so no in-force question to answer: the note would be about a choice that
    // could not exist. Reachable in life — an uncaptured cell whose selection document read fails
    // — so the guard is right and the fake is given what it needs to reach it.
    const { core, answer, refuseReads } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    refuseReads();

    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);
    const opening = panel.open(1 as NodeId);
    answer(1, []);
    await opening;

    expect(ui.stripHeading.textContent).toBe('Nothing captured here yet.');
  });

  it('shows the later of two quick picks, not whichever refresh ran first', async () => {
    // Both clicks come from one render, so both carry that render's ticket. The first write to
    // land refreshes the strip and bumps the ticket; the second then finds its own ticket stale
    // and returns without refreshing — leaving the strip showing the earlier pick while the core
    // holds the later one. The screen and the build disagree, which is the exact failure this
    // whole change was made to end.
    //
    // The ticket is the wrong question here. It guards *this render* against a late answer for a
    // different one; what a write needs to know is whether the cell is still on screen.
    const { core, answer, recorded, writesInFlight } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);

    const opening = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await opening;

    const buttons = () => [...ui.strip.querySelectorAll('button')];
    const pressedIndex = () =>
      buttons().findIndex((button) => button.getAttribute('aria-pressed') === 'true');

    // Two picks from the same strip, in flight together: the second one is what the user meant.
    // The *second* candidate, deliberately — index 0 is what `candidateStrip` marks in force when
    // there is no selection at all, so a panel that had stopped asking the core and gone back to
    // the ranking would satisfy the assertion below without holding anything.
    buttons()[0].click();
    buttons()[1].click();
    expect(writesInFlight()).toBe(2);

    // Released one at a time, and settled in between. Releasing both at once would let the
    // second write land before the first refresh read anything, and the assertion would pass
    // against the bug.
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await recorded();
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await recorded();

    expect(pressedIndex()).toBe(1);
  });

  it('shows what the core holds when two picks for one cell land out of order', async () => {
    // The worker answers in the order it was asked *today*, and this panel's comments refuse to
    // rely on that — so the guard that decides which write refreshes must not rely on it either.
    // Released newest-first, the core ends up holding the *older* pick, and the strip has to agree
    // with the core rather than with whichever write happened to be issued last.
    const { core, answer, recordedAt, writesInFlight } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);

    const opening = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await opening;

    const buttons = [...ui.strip.querySelectorAll('button')] as HTMLButtonElement[];
    buttons[0].click();   // candidate 10, issued first
    buttons[1].click();   // candidate 11, issued second
    expect(writesInFlight()).toBe(2);

    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await recordedAt(1);  // the later pick lands first: the core holds 11
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await recordedAt(0);  // the earlier pick lands last: the core holds 10

    const pressed = [...ui.strip.querySelectorAll('button')]
      .map((button) => button.getAttribute('aria-pressed'));
    expect(pressed).toEqual(['true', 'false']);
  });

  it('refreshes a cell you came back to while its pick was still in flight', async () => {
    // A write outliving a trip to another cell. On the way back `opened` names this cell again, so
    // the only thing that can suppress the refresh is the count — and a count shared with the
    // other cell's pick would suppress it, leaving the strip showing the ranking's own choice
    // while the core holds the user's.
    const { core, answer, recordedAt } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);

    const first = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await first;
    [...ui.strip.querySelectorAll('button')][1].click();   // cell 1 → 11, left in flight

    const second = panel.open(2 as NodeId);
    answer(2, [candidate(20, 2), candidate(21, 2)]);
    await second;
    [...ui.strip.querySelectorAll('button')][0].click();   // cell 2 → 20, also in flight

    const back = panel.open(1 as NodeId);
    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await back;

    answer(1, [candidate(10, 1), candidate(11, 1)]);
    await recordedAt(0);   // cell 1's write lands, and cell 1 is what is on screen

    const pressed = [...ui.strip.querySelectorAll('button')]
      .map((button) => button.getAttribute('aria-pressed'));
    expect(pressed).toEqual(['false', 'true']);
  });

  it('still draws the cell you asked for when the replies are in order', async () => {
    const { core, answer } = deferredCore();
    const { paint } = recordingPainter();
    const ui = elements();
    const panel = createReviewPanel(ui, core, paint);
    panel.show(plan, coverage);

    const opening = panel.open(3 as NodeId);
    answer(3, [candidate(30, 3)]);
    await opening;

    expect(ui.strip.querySelectorAll('button')).toHaveLength(1);
  });
});

describe('paintPreviewOnCanvas', () => {
  const shaped = (width: number, height: number, bytes: number) => ({
    frame: 1 as FramePreview['frame'],
    width,
    height,
    format: 'RGBA8' as FramePreview['format'],
    pixels: new Uint8Array(bytes),
  });

  it('accepts a preview whose pixels are the shape its dimensions claim', () => {
    expect(previewIsDrawable(shaped(4, 3, 4 * 3 * 4))).toBe(true);
  });

  it('refuses one whose dimensions are not whole numbers', () => {
    // A fraction survives the count check and then does not survive `ImageData`: WebIDL truncates
    // 1.5 to 1 on the way in and *then* compares the length, so 1.5x8 with 48 pixels passes here
    // and throws there — a throw being exactly what this guard exists to keep out of a loop.
    // `Number.isInteger` also rules out NaN and the infinities, which arrive the same way.
    expect(previewIsDrawable(shaped(1.5, 8, Math.round(1.5 * 8 * 4)))).toBe(false);
    expect(previewIsDrawable(shaped(4, 2.5, Math.round(4 * 2.5 * 4)))).toBe(false);
    expect(previewIsDrawable(shaped(Number.NaN, 8, 0))).toBe(false);
    expect(previewIsDrawable(shaped(Number.POSITIVE_INFINITY, 8, 0))).toBe(false);
  });

  it('refuses one whose pixel count does not match its dimensions', () => {
    // `ImageData` throws on a mismatch, and `fillPreviews` is a loop: a throw would stop every
    // row after this one from being drawn. The core would have to be wrong for this to happen —
    // which is the reason to check it, because a decode can succeed on a payload that is
    // internally inconsistent and a list is the wrong place to discover that.
    expect(previewIsDrawable(shaped(4, 3, 4 * 3 * 4 - 4))).toBe(false);
    expect(previewIsDrawable(shaped(4, 3, 4 * 3 * 4 + 4))).toBe(false);
    expect(previewIsDrawable(shaped(0, 3, 0))).toBe(false);
    expect(previewIsDrawable(shaped(4, 0, 0))).toBe(false);
  });

  it('marks a malformed preview rather than throwing', () => {
    // Told apart from `missing`, which is what a canvas this environment will not give a 2D
    // context for gets — and that is every canvas under jsdom, so a single marker would make
    // this assertion pass without the guard existing at all.
    const canvas = document.createElement('canvas');
    expect(() => paintPreviewOnCanvas(canvas, shaped(4, 3, 7))).not.toThrow();
    expect(canvas.dataset.preview).toBe('malformed');
  });

  it('leaves a well-formed preview to the canvas, whatever the canvas does about it', () => {
    const canvas = document.createElement('canvas');
    paintPreviewOnCanvas(canvas, shaped(4, 3, 4 * 3 * 4));
    expect(canvas.dataset.preview).not.toBe('malformed');
  });
});
