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

/**
 * A call to the core that answers rather than throws.
 *
 * Every call this panel makes crosses a `postMessage`, and `remote-core`'s `die()` rejects every
 * call in flight and every call after it once the worker is gone — a worker script that 404s, a
 * throw at its top level, a reply that cannot be read. An unhandled rejection here would leave the
 * strip without the rows it was about to draw and without a word about why; on any open but the
 * first it would leave the *previous* cell's rows standing under the new cell's identity, which is
 * worse still. A rejection is the same news as `{ok: false}` and is told the same way.
 *
 * The reason goes to the console, because the screen cannot carry it. Three mechanisms reject
 * here — the worker is dead, it answered `failed`, or it answered something that is not a reply
 * — and the second is how the interesting ones arrive, because the facade runs inside the worker
 * and its throws are caught there: a version skew (`facade.ts` throws "the bundle and the core
 * disagree" by name, precisely so it will not surface as a silent empty result somewhere else)
 * and a failed allocation both come back as `failed`.
 *
 * What they share is not that a retry cannot help — a failed allocation is transient, and a later
 * pick does re-issue the read. It is that none of them is the *document's* fault, so a heading
 * that says the document could not be read is wrong about all of them, and the rows it leaves
 * clickable are the wrong advice for all of them. The reason a user cannot act on belongs where a
 * developer will look; `worker.ts` already reports its own unhandled failures this way.
 */
function answering<T>(call: Promise<Answered<T>>): Promise<Answered<T>> {
  return call.then(undefined, (reason: unknown) => {
    console.error('sphanorama review: a call to the core did not answer', reason);
    return { ok: false } as const;
  });
}

export interface ReviewCore {
  candidates(node: NodeId): Promise<Answered<Candidate[]>>;
  candidatePreview(
    node: NodeId, candidate: CandidateId, maxEdge: number): Promise<Answered<FramePreview>>;
  setSelection(node: NodeId, candidate: CandidateId): Promise<{ readonly ok: boolean }>;
  /** What the core has recorded for this cell. A zero candidate means nobody has chosen. */
  selection(node: NodeId): Promise<Answered<CandidateId>>;
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
 * Whether a preview's pixels are the shape its dimensions claim.
 *
 * `ImageData` throws on a mismatch, and a throw here would take the rest of the strip with it —
 * `fillPreviews` is a loop, so one bad row would stop every row after it from being drawn. The
 * core would have to be wrong for this to be false, which is exactly why it is checked: the
 * decode can succeed on a payload that is internally inconsistent, and a list is the wrong place
 * to find that out.
 */
export function previewIsDrawable(preview: FramePreview): boolean {
  // Whole numbers, not merely positive ones. `ImageData` puts its dimensions through WebIDL's
  // integer conversion *before* it compares them to the pixel count, so a width of 1.5 becomes 1
  // and a payload that satisfied the count check a moment ago throws — which is the one outcome
  // this guard exists to keep out of a loop. `Number.isInteger` rules out NaN and the infinities
  // on the way past, since they arrive through the same door.
  return Number.isInteger(preview.width) && Number.isInteger(preview.height)
    && preview.width > 0 && preview.height > 0
    && preview.pixels.length === preview.width * preview.height * 4;
}

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
  // Before the context, so that a preview which could never be drawn says so rather than being
  // reported as a canvas this browser would not give one for. They are different problems and
  // only one of them is about the picture.
  if (!previewIsDrawable(preview)) {
    canvas.dataset.preview = 'malformed';
    return;
  }
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
  // Which open() call the strip belongs to. Every call this panel makes crosses a postMessage to
  // the worker the core runs in (ADR 0019), so two taps in quick succession are two answers in
  // flight with nothing promising they come back in the order they were asked. Drawing from
  // whichever settles last would put one cell's candidates under another cell's heading — and
  // under the map's pressed dot, which is set before the await and so always names the later tap.
  let latest = 0;
  // The open cell's previews, so that recording a pick does not read its pixels again. Refreshing
  // the strip re-opens the cell, and every preview it asks for again faults a spilled frame back
  // into the heap and sends it away afterwards — around 350 ms of file work for a cell of eight,
  // to redraw pictures the page is already holding.
  //
  // Bounded two ways, because one is not enough. It is dropped when another cell is opened —
  // holding every cell's thumbnails would be the review client's own version of the memory
  // problem the reduction exists to solve — and pruned to the strip on every open, because a
  // retake gives *this* cell new identities and the ones it replaced would otherwise be held for
  // as long as the user stayed on it.
  let previewsFor: NodeId | null = null;
  const previews = new Map<CandidateId, FramePreview>();
  // How many picks are still in flight for each cell. The last of them to *land* refreshes, and
  // the ones before it stand down: their refresh would read a core that another write is about to
  // change, paying a round trip to paint something already out of date.
  //
  // Outstanding rather than newest, and that distinction is the whole of it. "Am I the newest pick
  // issued" is only the same question as "is the core finished changing" while answers come back
  // in the order they were asked — which this panel does not get to assume, and said so two
  // comments ago while relying on it. Released the other way round, the newest write refreshes off
  // a core an older one is still about to overwrite, and the older one then stands down: the strip
  // keeps a pick the core does not hold, until the user leaves the cell and comes back. Counting
  // what is outstanding asks the question that actually decides it, in any order.
  //
  // Per cell, because it is a fact about the cell. One counter shared by the panel would let a
  // pick in another cell stand down this cell's own.
  const outstanding = new Map<NodeId, number>();

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
      const answered = await answering(
        core.candidatePreview(node, candidate, PREVIEW_MAX_EDGE));
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
    // Asked together, because they are one question with two halves — what is here, and which of
    // it is in force — and asking them in sequence would put a second round trip in front of the
    // first row of pixels for no reason.
    const [answered, recorded] = await Promise.all(
      [answering(core.candidates(node)), answering(core.selection(node))]);
    // A cell the user has already left. Returning without touching the DOM leaves the strip
    // showing what was asked for most recently, which is the only cell the rest of the panel
    // claims to be showing.
    if (ticket !== latest) return;
    elements.strip.replaceChildren();
    if (!answered.ok) {
      elements.stripHeading.textContent = 'That cell could not be read.';
      return;
    }

    // Dropped here rather than before the await, so a tap that is superseded before it ever
    // paints does not take the thumbnails of the cell still on screen with it. Clearing up there
    // costs the ~350 ms of spill faulting below to fetch back pictures the page never stopped
    // needing, for a render that was cancelled two lines ago.
    if (previewsFor !== node) {
      previews.clear();
      previewsFor = node;
    }

    // Zero — the core's answer for "nobody has chosen here" — is passed straight through rather
    // than translated: it is an identity no candidate can have, so `candidateStrip` lands it in
    // the same place as a choice whose candidate a retake forgot, which is where it belongs.
    // A read that *failed* is not that answer and is not folded into it: nothing is marked in
    // force, and the heading below says why. Showing the ranking's pick as though it were what
    // the build will use would be the screen disagreeing with the build without a word anywhere,
    // which is the failure the read-back exists to end (ADR 0040).
    const entries = candidateStrip(answered.value, recorded.ok ? recorded.value : 'unreadable');
    // Pruned to what the strip is about to show. Clearing on a *change* of cell is not enough on
    // its own: a retake gives this same cell new identities and the old ones never come back, so
    // their pictures would be held for as long as the user stays here — 48 KB each, growing with
    // every retake, in the client whose whole reason for reducing was to stop holding frames.
    for (const candidate of previews.keys()) {
      if (!entries.some((entry) => entry.candidate === candidate)) previews.delete(candidate);
    }
    const counted = entries.length === 0
      ? 'Nothing captured here yet.'
      : `${entries.length} candidate${entries.length === 1 ? '' : 's'}, best first.`;
    elements.stripHeading.textContent = recorded.ok || entries.length === 0
      ? counted
      : `${counted} Which one is in force could not be read.`;

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
          // Acquired here and released in the `finally`, which is what makes the balance
          // structural rather than a rule to remember: a cell whose release does not run is a
          // cell that never refreshes again, and `finally` covers every way out of the block —
          // a rejection, a synchronous throw, and an early `return` some later edit puts in the
          // middle. `.catch` alone covered only the first of those. The release is also
          // deliberately before the guard below: walking away from a cell releases its count
          // rather than leaking it.
          outstanding.set(node, (outstanding.get(node) ?? 0) + 1);
          let left = 0;
          try {
            // Neither a refusal nor a failure is an outcome this has to tell apart. Nothing is
            // remembered here: re-opening asks the core what is recorded, so a write that was
            // refused — or one that never reached a worker at all — leaves the previous choice in
            // force without this having to know which happened, and what the strip shows is what
            // the build will use rather than a second copy kept on this side that a reload would
            // forget. The reason still goes to the console, for the same reason `answering` sends
            // one: the screen cannot carry it and a developer needs it.
            await core.setSelection(node, entry.candidate);
          } catch (reason: unknown) {
            console.error('sphanorama review: recording a pick did not answer', reason);
          } finally {
            // The entry is always here. This handler put it in before the await, and an entry is
            // removed only when its count reaches zero, which cannot have happened while this
            // write is still counted in it — so there is no missing-entry case to defend, and
            // writing one would be a branch nothing can reach.
            const held = outstanding.get(node) as number;
            left = held - 1;
            if (left === 0) outstanding.delete(node);
            else outstanding.set(node, left);
          }

          // Two guards, answering two different questions, and neither is the render ticket: a
          // ticket belongs to one render, and two picks made in one render carry the same one.
          //
          // `opened` is the last cell *asked for*, which is not the cell currently painted — that
          // lags it by a round trip. Asked-for is deliberately the question. A write that lands
          // while the user is already on their way to another cell must not refresh, because a
          // refresh takes the newest ticket and would paint this cell over the one they are going
          // to. The strip they are walking away from does go briefly stale, and it corrects itself
          // the moment they come back, because coming back reads the core rather than a copy.
          //
          // `left` is whether this cell has any pick still in flight. Without it two quick picks
          // cost two refreshes, the first of them reading a core the second is about to change.
          if (opened !== node || left > 0) return;
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
