// The panel's own decisions, which are about *when* an answer is allowed to reach the DOM. What
// the strip says is candidateStrip's business and what the map looks like is reviewed by eye;
// what is here is the ordering the panel has to survive because every call it makes crosses a
// worker.
import { describe, expect, it } from 'vitest';

import { createReviewPanel, type ReviewCore, type ReviewElements } from './panel';
import type {
  Candidate, CapturePlan, CoverageState, NodeId,
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

/** A core whose answers are released by hand, so an out-of-order reply can be staged. */
function deferredCore() {
  const pending = new Map<number, (candidates: Candidate[]) => void>();
  // An answer staged before the call that will collect it. The panel re-opens a cell from inside
  // a click handler, so a test cannot always be holding the promise at the moment it wants to
  // decide what comes back.
  const queued = new Map<number, Candidate[]>();
  let recording: (() => void) | null = null;
  const core: ReviewCore = {
    candidates: (node: NodeId) => new Promise((resolve) => {
      const staged = queued.get(node as number);
      if (staged !== undefined) {
        queued.delete(node as number);
        resolve({ ok: true, value: staged });
        return;
      }
      pending.set(node as number, (value) => resolve({ ok: true, value }));
    }),
    setSelection: () => new Promise((resolve) => { recording = () => resolve({ ok: true }); }),
  };
  return {
    core,
    answer(node: number, candidates: Candidate[]) {
      const release = pending.get(node);
      if (release === undefined) { queued.set(node, candidates); return; }
      pending.delete(node);
      release(candidates);
    },
    /** Lets the in-flight SetSelection through, and waits for what it set off to settle. */
    async recorded() {
      if (recording === null) throw new Error('no selection is in flight');
      const release = recording;
      recording = null;
      release();
      // Enough turns for the click handler to resume and for the open() it may then await to run
      // its own continuation.
      for (let turn = 0; turn < 4; turn += 1) await Promise.resolve();
    },
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
    const ui = elements();
    const panel = createReviewPanel(ui, core);
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
    const ui = elements();
    const panel = createReviewPanel(ui, core);
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
    const ui = elements();
    const panel = createReviewPanel(ui, core);
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

  it('still draws the cell you asked for when the replies are in order', async () => {
    const { core, answer } = deferredCore();
    const ui = elements();
    const panel = createReviewPanel(ui, core);
    panel.show(plan, coverage);

    const opening = panel.open(3 as NodeId);
    answer(3, [candidate(30, 3)]);
    await opening;

    expect(ui.strip.querySelectorAll('button')).toHaveLength(1);
  });
});
