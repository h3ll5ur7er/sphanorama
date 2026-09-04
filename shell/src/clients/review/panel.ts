/**
 * The review panel's DOM: the coverage map, the candidate strip, and the click that overrides a
 * pick.
 *
 * It lives with the review client rather than in the capture client's entry point, and it holds
 * no judgement of its own — where a cell sits comes from `mapCoverage`, what order the strip is
 * in comes from the core's ranking, and what a click means is `SetSelection` (UC-3). What is here
 * is elements and events.
 *
 * The core arrives as three functions rather than the facade, so the panel can be driven without
 * a worker and so the only calls it can possibly make are the three it is meant to.
 */
import type {
  Candidate, CandidateId, CapturePlan, CoverageState, NodeId,
} from '../../../../contracts/ts/contracts';
import { candidateStrip } from './candidates';
import { mapCoverage } from './coverage-map';

export interface ReviewElements {
  panel: HTMLElement;
  map: HTMLElement;
  stripHeading: HTMLElement;
  strip: HTMLElement;
}

/**
 * What the facade hands back: a value, or a failure carrying a status this panel does not read.
 * Narrowed rather than optional so `answered.value` cannot be touched on the failing branch.
 */
type Answered<T> = { readonly ok: true; readonly value: T } | { readonly ok: false };

export interface ReviewCore {
  candidates(node: NodeId): Promise<Answered<Candidate[]>>;
  setSelection(node: NodeId, candidate: CandidateId): Promise<{ readonly ok: boolean }>;
}

export interface ReviewPanel {
  /** Redraws the map. Called when coverage changes, which is when a cell completes. */
  show(plan: CapturePlan, coverage: CoverageState): void;
  /** Opens a cell's strip, as clicking its dot does. Exposed so a test can drive it. */
  open(node: NodeId): Promise<void>;
}

const percent = (fraction: number) => `${(fraction * 100).toFixed(2)}%`;

export function createReviewPanel(elements: ReviewElements, core: ReviewCore): ReviewPanel {
  // Which cell's strip is open, so a redraw of the map keeps it open rather than closing the
  // thing the user was reading every time another cell completes.
  let opened: NodeId | null = null;
  // The manual override per cell, as recorded through SetSelection. Held here because the core
  // has nowhere to answer "what is selected" from — ProjectManager writes the selection and no
  // contract reads it back, so a reload forgets it. Named rather than hidden: see the ADR.
  const chosen = new Map<NodeId, CandidateId>();

  async function open(node: NodeId): Promise<void> {
    opened = node;
    const answered = await core.candidates(node);
    elements.strip.replaceChildren();
    if (!answered.ok) {
      elements.stripHeading.textContent = 'That cell could not be read.';
      return;
    }

    const entries = candidateStrip(answered.value, chosen.get(node) ?? null);
    elements.stripHeading.textContent = entries.length === 0
      ? 'Nothing captured here yet.'
      : `${entries.length} candidate${entries.length === 1 ? '' : 's'}, best first.`;

    for (const entry of entries) {
      const item = document.createElement('li');
      const button = document.createElement('button');
      button.type = 'button';
      button.setAttribute('aria-pressed', String(entry.isInForce));
      const notes = [`sharpness ${entry.quality.sharpness.toFixed(3)}`,
                     `exposure ${entry.quality.exposureAgreement.toFixed(2)}`];
      if (entry.isAutomaticPick) notes.push('the core’s pick');
      button.textContent = `#${entry.candidate} — ${notes.join(' · ')}`;
      button.addEventListener('click', () => {
        void (async () => {
          const set = await core.setSelection(node, entry.candidate);
          // Recorded only once the core has taken it, so the strip cannot show a choice that was
          // refused. A failure leaves the previous one in force, which is what the build uses.
          if (set.ok) chosen.set(node, entry.candidate);
          await open(node);
        })();
      });
      item.append(button);
      elements.strip.append(item);
    }
  }

  return {
    show(plan, coverage) {
      elements.panel.dataset.active = 'true';
      elements.map.replaceChildren();
      for (const cell of mapCoverage(plan, coverage)) {
        const dot = document.createElement('button');
        dot.type = 'button';
        dot.className = 'cell';
        dot.dataset.state = cell.state;
        dot.style.left = percent(cell.x);
        dot.style.top = percent(cell.y);
        dot.setAttribute('aria-pressed', String(cell.node === opened));
        dot.setAttribute(
          'aria-label',
          `Cell ${cell.node}, ${cell.state === 'hole' ? 'not captured' : 'captured'}`);
        dot.addEventListener('click', () => { void open(cell.node); });
        elements.map.append(dot);
      }
    },
    open,
  };
}
