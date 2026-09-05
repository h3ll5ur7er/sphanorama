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
  Candidate, CandidateId, CapturePlan, CoverageState, FramePreview, NodeId,
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
 * The long edge the strip asks the core for.
 *
 * A thumbnail sits in a 4rem box, so 128 covers it on a 2x screen and nothing is gained by
 * asking for more: the core caps this at `kFramePreviewMaxEdge` because what crosses is
 * uncompressed RGBA, and a whole cell at 128 is 384 KB where the frames themselves would be
 * 39 MB (ADR 0038). How large the picture *looks* is CSS; this is how much of it travels.
 */
export const PREVIEW_MAX_EDGE = 128;

/**
 * What the facade hands back: a value, or a failure carrying a status this panel does not read.
 * Narrowed rather than optional so `answered.value` cannot be touched on the failing branch.
 */
type Answered<T> = { readonly ok: true; readonly value: T } | { readonly ok: false };

export interface ReviewCore {
  candidates(node: NodeId): Promise<Answered<Candidate[]>>;
  candidatePreview(
    node: NodeId, candidate: CandidateId, maxEdge: number): Promise<Answered<FramePreview>>;
  setSelection(node: NodeId, candidate: CandidateId): Promise<{ readonly ok: boolean }>;
}

/**
 * Somewhere to put a decoded preview.
 *
 * An interface rather than a call to `getContext` inline, for the same reason `DrawTarget` is one
 * in the camera path: a 2D context cannot be had outside a browser, and what is worth testing
 * here is *which* candidate gets painted and *when* — the ordering the panel has to survive
 * because every call it makes crosses a worker. That the browser can put an ImageData on a canvas
 * is not this client's claim to make.
 */
export type PaintPreview = (canvas: HTMLCanvasElement, preview: FramePreview) => void;

/**
 * The real painter: RGBA8 straight into an `ImageData` and onto the canvas.
 *
 * No decode step anywhere, which is why the core hands back raw pixels rather than an encoded
 * image — there is no codec on this path and adding one to draw a thumbnail would be a lot of
 * bytes of JavaScript to save a few hundred kilobytes of transfer (ADR 0038).
 *
 * The pixels are copied into a clamped array rather than viewed through one. A view would be free
 * and it is not available: `ImageData` will not take a buffer that might be shared, and the
 * decoded bytes are a subarray of whatever buffer the response arrived in — an ordinary one
 * today, a `SharedArrayBuffer` in the threaded build (ADR 0011). At a long edge of 128 the copy
 * is 48 KB, which is the size the reduction was chosen to make not worth optimising.
 */
export const paintPreviewOnCanvas: PaintPreview = (canvas, preview) => {
  const context = canvas.getContext('2d');
  if (context === null) {
    // Marked rather than thrown: the strip is a list, and one row that cannot be drawn must not
    // stop the rows after it from being.
    canvas.dataset.preview = 'missing';
    return;
  }
  canvas.width = preview.width;
  canvas.height = preview.height;
  const pixels = new Uint8ClampedArray(preview.pixels);
  context.putImageData(new ImageData(pixels, preview.width, preview.height), 0, 0);
  canvas.dataset.preview = 'ready';
};

export interface ReviewPanel {
  /** Redraws the map. Called when coverage changes, which is when a cell completes. */
  show(plan: CapturePlan, coverage: CoverageState): void;
  /** Opens a cell's strip, as clicking its dot does. Exposed so a test can drive it. */
  open(node: NodeId): Promise<void>;
}

const percent = (fraction: number) => `${(fraction * 100).toFixed(2)}%`;

export function createReviewPanel(
  elements: ReviewElements, core: ReviewCore, paint: PaintPreview): ReviewPanel {
  // Which cell's strip is open, so a redraw of the map keeps it open rather than closing the
  // thing the user was reading every time another cell completes.
  let opened: NodeId | null = null;
  // The manual override per cell, as recorded through SetSelection. Held here because the core
  // has nowhere to answer "what is selected" from — ProjectManager writes the selection and no
  // contract reads it back, so a reload forgets it. Named rather than hidden: see the ADR.
  const chosen = new Map<NodeId, CandidateId>();
  // Which open() call the strip belongs to. Every call this panel makes crosses a postMessage to
  // the worker the core runs in (ADR 0019), so two taps in quick succession are two answers in
  // flight with nothing promising they come back in the order they were asked. Drawing from
  // whichever settles last would put one cell's candidates under another cell's heading — and
  // under the map's pressed dot, which is set before the await and so always names the later tap.
  let latest = 0;
  // The open cell's previews, so that recording a pick does not read its pixels again. Refreshing
  // the strip re-opens the cell, and every preview it asks for again faults a spilled frame back
  // into the heap and sends it away afterwards — around 350 ms of file work for a cell of eight,
  // to redraw pictures the page is already holding. Dropped the moment another cell is opened:
  // holding every cell's thumbnails would be the review client's own version of the memory
  // problem the reduction exists to solve.
  let previewsFor: NodeId | null = null;
  const previews = new Map<CandidateId, FramePreview>();

  /**
   * Fills in the strip's pictures, one candidate at a time.
   *
   * Sequential rather than all at once, so the strip fills in as the answers arrive and a cell
   * the user has already left stops costing anything at the next candidate rather than at the
   * last one.
   */
  async function fillPreviews(
    node: NodeId, ticket: number, rows: ReadonlyMap<CandidateId, HTMLCanvasElement>,
  ): Promise<void> {
    for (const [candidate, canvas] of rows) {
      const held = previews.get(candidate);
      if (held !== undefined) {
        paint(canvas, held);
        continue;
      }
      const answered = await core.candidatePreview(node, candidate, PREVIEW_MAX_EDGE);
      // A cell the user has left. Nothing is drawn and nothing is remembered: the ticket is
      // checked before the cache is written, so a late answer cannot seed the cell that replaced
      // it with another cell's frames.
      if (ticket !== latest) return;
      if (!answered.ok) {
        // A replace-retake forgets a cell's frames, so a strip fetched a moment ago can name a
        // candidate that has since gone. The row stays, with its scores, saying only that the
        // picture is not there — which is more than the strip could say before it had one.
        canvas.dataset.preview = 'missing';
        continue;
      }
      previews.set(candidate, answered.value);
      paint(canvas, answered.value);
    }
  }

  async function open(node: NodeId): Promise<void> {
    opened = node;
    const ticket = ++latest;
    if (previewsFor !== node) {
      previews.clear();
      previewsFor = node;
    }
    const answered = await core.candidates(node);
    // A cell the user has already left. Returning without touching the DOM leaves the strip
    // showing what was asked for most recently, which is the only cell the rest of the panel
    // claims to be showing.
    if (ticket !== latest) return;
    elements.strip.replaceChildren();
    if (!answered.ok) {
      elements.stripHeading.textContent = 'That cell could not be read.';
      return;
    }

    const entries = candidateStrip(answered.value, chosen.get(node) ?? null);
    elements.stripHeading.textContent = entries.length === 0
      ? 'Nothing captured here yet.'
      : `${entries.length} candidate${entries.length === 1 ? '' : 's'}, best first.`;

    const rows = new Map<CandidateId, HTMLCanvasElement>();
    for (const entry of entries) {
      const item = document.createElement('li');
      const button = document.createElement('button');
      button.type = 'button';
      button.setAttribute('aria-pressed', String(entry.isInForce));
      const thumbnail = document.createElement('canvas');
      thumbnail.className = 'thumb';
      // Marked pending from the start, so the box has its shape before any pixels arrive and the
      // strip does not reflow under the reader's finger as each answer lands.
      thumbnail.dataset.preview = 'pending';
      const notes = [`sharpness ${entry.quality.sharpness.toFixed(3)}`,
                     `exposure ${entry.quality.exposureAgreement.toFixed(2)}`];
      if (entry.isAutomaticPick) notes.push('the core’s pick');
      const label = document.createElement('span');
      label.className = 'strip-label';
      label.textContent = `#${entry.candidate} — ${notes.join(' · ')}`;
      button.append(thumbnail, label);
      button.addEventListener('click', () => {
        void (async () => {
          const set = await core.setSelection(node, entry.candidate);
          // Recorded only once the core has taken it, so the strip cannot show a choice that was
          // refused. A failure leaves the previous one in force, which is what the build uses.
          if (set.ok) chosen.set(node, entry.candidate);
          // Re-opened so the strip shows what is now in force — but only while this strip is
          // still the one on screen. SetSelection crosses the worker too, so the user can have
          // opened another cell in the meantime, and re-opening then hauls them back: `opened` is
          // set before the await inside open(), so the map's pressed dot moves the instant the
          // late refresh starts, before any answer has even come back for it.
          if (ticket !== latest) return;
          await open(node);
        })();
      });
      item.append(button);
      elements.strip.append(item);
      rows.set(entry.candidate, thumbnail);
    }
    await fillPreviews(node, ticket, rows);
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
