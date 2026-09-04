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
  const core: ReviewCore = {
    candidates: (node: NodeId) => new Promise((resolve) => {
      pending.set(node as number, (value) => resolve({ ok: true, value }));
    }),
    setSelection: async () => ({ ok: true }),
  };
  return {
    core,
    answer(node: number, candidates: Candidate[]) {
      const release = pending.get(node);
      if (release === undefined) throw new Error(`nothing is waiting on cell ${node}`);
      pending.delete(node);
      release(candidates);
    },
  };
}

const plan = { nodes: [], spec: {} } as unknown as CapturePlan;
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
