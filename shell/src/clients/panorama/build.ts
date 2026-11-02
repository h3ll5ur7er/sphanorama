/**
 * The build client: one button that turns the capture into a preview panorama, the progress while
 * it runs, and the picture when it is done (ADR 0071).
 *
 * It decides nothing about the panorama. Which frames, which pairs and which lens are the core's
 * (ADR 0070); what is here is when to ask, how often, and what to put on the page with the answer.
 * The core arrives as `start`, `poll`, `preview`, `gaps` and `fill` rather than the facade, so this
 * can be driven without a worker and so those are the only calls it can make. `fill` is the capture
 * manager's, and this is the client that sequences the two: the managers never call each other, so
 * the gaps one finds reach the other through here (ADR 0086).
 */
import type {
  BuildId, BuildProgress, BuildStage, CapturePlan, CoverageGap, FoundGaps, FramePreview, Status,
} from '../../../../contracts/ts/contracts';

/** A value, or a failure carrying the core's status — which, unlike the review strip, this shows. */
type Answered<T> = { readonly ok: true; readonly value: T }
  | { readonly ok: false; readonly status?: Status };

export interface BuildCore {
  start(): Promise<Answered<BuildId>>;
  poll(build: BuildId): Promise<Answered<BuildProgress>>;
  preview(build: BuildId): Promise<Answered<FramePreview>>;
  gaps(build: BuildId): Promise<Answered<FoundGaps>>;
  // Handed the build's answer whole, with the capture it judged, which the core holds a fill to.
  fill(found: FoundGaps): Promise<Answered<CapturePlan>>;
}

export interface BuildElements {
  button: HTMLButtonElement;
  status: HTMLElement;
  progress: HTMLProgressElement;
  canvas: HTMLCanvasElement;
  /** Offered only while the last build's gaps are there to fill. */
  fill: HTMLButtonElement;
}

/**
 * Draws a preview onto the canvas and marks it `data-preview="ready"` when it did — the contract
 * `paintPreviewOnCanvas` keeps. A preview it could not draw is marked otherwise and the canvas's
 * pixels are left alone, which is why this client reads the mark rather than assuming the paint.
 */
export type PaintPanorama = (canvas: HTMLCanvasElement, preview: FramePreview) => void;

/**
 * What the page says a build is doing. Only the stages the build reports today are named; the rest
 * belong to a full render that does not exist, and a label for each would be a promise of one.
 */
const doing: Partial<Record<BuildStage, string>> = {
  Queued: 'starting',
  Features: 'finding features in each frame',
  PairwiseMatching: 'matching neighbouring frames',
  GlobalSolve: 'solving for where each frame points',
  CoverageCheck: 'checking the sphere for gaps',
  Projecting: 'drawing the panorama',
};

const why = (status: Status | undefined) => status?.detail || status?.code || 'no reason given';

/**
 * The gaps as a sentence: how much of the sphere is open, and how many frames would complete the
 * capture's plan. Each entry is a fill frame's target, not a hole — one hole wider than a frame is
 * several (ADR 0084) — so the count is of frames. The share is of the whole sphere, as
 * `solidAngleFraction` is; only the plan's directions are judged, which for a band without caps is
 * not the whole sphere, so the frames and an empty answer are said of the plan. Where they are is
 * for the capture to lead the user to.
 */
export function describeGaps(gaps: readonly CoverageGap[]): string {
  if (gaps.length === 0) return 'Nothing the capture planned is missing.';
  const percent = 100 * gaps.reduce((sum, g) => sum + g.solidAngleFraction, 0);
  // A seam a few directions wide is open all the same, and "0%" would read as nothing.
  const share = percent < 0.05 ? 'Under 0.1%' : `About ${percent.toFixed(1)}%`;
  const frames = gaps.length === 1 ? '1 more frame' : `${gaps.length} more frames`;
  return `${share} of the sphere is open; ${frames} would complete the capture's plan.`;
}

/**
 * A wait before each call to the core that does not end while `busy` says the capture needs it.
 *
 * Each call is work on the core's one thread — a poll is a build step, a start reads the capture,
 * the preview is copied out — and a capture's tick waits behind it. So a burst, which advances one
 * frame a tick (ADR 0018), would be spread across build calls with its exposure locks held, and its
 * frames would stop being the comparable set the burst is for. Holding every call back until the
 * burst is done costs the build that long; the one call already in flight when a burst arms is the
 * overlap left (ADR 0071). It yields once even when nothing is busy, so the page paints the
 * progress between steps.
 */
export function yieldWhile(busy: () => boolean, retryMs = 50): () => Promise<void> {
  return () => new Promise((resolve) => {
    const check = () => { if (busy()) setTimeout(check, retryMs); else resolve(); };
    setTimeout(check, 0);
  });
}

/** Where the capture is, as far as whether a build may take the core's thread. */
export interface CaptureState {
  readonly stopped: boolean;
  readonly arming: boolean;
  readonly armed: boolean;
  readonly firing: boolean;
}

/**
 * Whether a burst is under way that the build's next call would split. Not once the capture has
 * stopped: it has no next tick to finish a burst with, so whatever its last tick left set would
 * hold a build for the life of the tab — and a camera taken away mid-burst leaves `firing` set.
 */
export function captureNeedsCore(capture: CaptureState): boolean {
  return !capture.stopped && (capture.arming || capture.armed || capture.firing);
}

/**
 * Builds when asked, and says how it is going.
 *
 * `yieldToPage` runs before every call to the core. Each poll is one step of the build on the core's
 * thread (ADR 0070), so the core is held for a step and never for a build; yielding between calls is
 * what lets the page paint the progress and the capture's ticks reach the core. `yieldWhile` is how
 * the page holds the build back while a burst needs the core.
 */
export function createBuildClient(
  ui: BuildElements, core: BuildCore, paint: PaintPanorama,
  yieldToPage: () => Promise<void> = yieldWhile(() => false),
  filled: (plan: CapturePlan) => void = () => {},
  // Whether the capture could act on a fill now: its loop is running, so there is a guide to lead
  // to the new cells, and every planned cell is shot, so a gap is not a cell the guide already
  // leads to (ADR 0086). Asked as a build starts, when its offer is made, when it is pressed and
  // when the fill has answered.
  canFill: () => boolean = () => true,
): { build(): Promise<void>; fill(): Promise<void> } {
  // One of the two at a time, and either holds both buttons: a build started under a fill judges
  // a plan the fill is changing, and a fill pressed under a build fills the last build's gaps.
  let running = false;
  // The last build's gaps, and only while its offer stands.
  let found: FoundGaps | null = null;
  ui.canvas.hidden = true;
  ui.fill.hidden = true;

  const hold = (held: boolean) => {
    ui.button.disabled = held;
    ui.fill.disabled = held;
  };

  async function run(): Promise<void> {
    ui.progress.value = 0;
    ui.status.textContent = 'Building a preview: starting.';
    await yieldToPage();
    // Asked of the capture `start` reads, which is the one the gaps are judged against: a cell shot
    // while the build runs is a gap in them, and filling it would lead back to where it was shot.
    const couldFill = canFill();
    const started = await core.start();
    if (!started.ok) {
      ui.status.textContent = `The build was refused: ${why(started.status)}.`;
      return;
    }
    for (;;) {
      await yieldToPage();
      const polled = await core.poll(started.value);
      if (!polled.ok) {
        ui.status.textContent = `The build could not be asked how it is going: ${why(polled.status)}.`;
        return;
      }
      const { stage, fraction, failure } = polled.value;
      ui.progress.value = fraction;
      if (stage === 'Failed') {
        ui.status.textContent = `The build failed: ${why(failure)}.`;
        return;
      }
      if (stage === 'Complete') break;
      ui.status.textContent = `Building a preview: ${doing[stage] ?? stage}.`;
    }
    await yieldToPage();
    const preview = await core.preview(started.value);
    if (!preview.ok) {
      ui.status.textContent = `The build finished, but its preview could not be read: ${why(preview.status)}.`;
      return;
    }
    paint(ui.canvas, preview.value);
    if (ui.canvas.dataset.preview !== 'ready') {
      ui.status.textContent = 'The build finished, but its preview could not be drawn.';
      return;
    }
    ui.canvas.hidden = false;
    const built = `Built: a ${preview.value.width} by ${preview.value.height} preview.`;
    ui.status.textContent = built;
    await yieldToPage();
    // Caught here rather than by `build`: the panorama is on screen, and a worker that goes away
    // now takes only this sentence with it, as a refused judgement does (ADR 0085).
    let gaps: Answered<FoundGaps>;
    try {
      gaps = await core.gaps(started.value);
    } catch (reason: unknown) {
      console.error('sphanorama build: the gaps call did not answer', reason);
      gaps = { ok: false, status: { detail: 'the core did not answer' } as Status };
    }
    ui.status.textContent = gaps.ok
      ? `${built} ${describeGaps(gaps.value.gaps)}`
      : `${built} Whether the sphere has gaps could not be judged: ${why(gaps.status)}.`;
    if (gaps.ok && gaps.value.gaps.length > 0 && couldFill && canFill()) {
      found = gaps.value;
      ui.fill.hidden = false;
    }
  }

  async function fillFound(offered: FoundGaps): Promise<void> {
    await yieldToPage();
    const answer = await core.fill(offered);
    if (!answer.ok) {
      ui.status.textContent = `The gaps could not be added to the plan: ${why(answer.status)}.`;
      return;
    }
    const count = offered.gaps.length;
    const cells = count === 1 ? '1 cell' : `${count} cells`;
    // Asked again, of the capture the fill was asked of: a loop that stopped while the fill was in
    // flight has no guide, though the cells are written down and a resume leads to them. Before
    // `filled`, which hands the loop a plan whose new cells are unshot, so asked after it this
    // would answer for a capture that can take no fill rather than for one with no guide.
    const toThem = canFill() ? 'follow the guide to them' : 'reload to be guided to them';
    found = null;
    ui.fill.hidden = true;
    filled(answer.value);
    ui.status.textContent = `Added ${cells} to the capture's plan: ${toThem}, then build again.`;
  }

  return {
    async build() {
      if (running) return;
      running = true;
      hold(true);
      // Out of sight until this build has something to show: last time's picture under this
      // time's progress, or this time's failure, would be read as this one's. The mark goes too, or
      // a painter that drew nothing this time would be read by last time's. And last time's gaps
      // with them, which this build may no longer find.
      ui.canvas.hidden = true;
      delete ui.canvas.dataset.preview;
      found = null;
      ui.fill.hidden = true;
      try {
        await run();
      } catch (reason: unknown) {
        // A rejection is a worker that went away or answered something unreadable; the screen says
        // the build stopped, and the reason goes where a developer will look.
        console.error('sphanorama build: a call to the core did not answer', reason);
        ui.status.textContent = 'The build stopped: the core did not answer.';
      } finally {
        running = false;
        hold(false);
      }
    },
    async fill() {
      if (running || found === null) return;
      if (!canFill()) {
        found = null;
        ui.fill.hidden = true;
        return;
      }
      running = true;
      hold(true);
      try {
        await fillFound(found);
      } catch (reason: unknown) {
        // The offer stands: nothing was added that the core said, and pressing again asks again.
        console.error('sphanorama build: the fill call did not answer', reason);
        ui.status.textContent = 'The gaps could not be added to the plan: the core did not answer.';
      } finally {
        running = false;
        hold(false);
      }
    },
  };
}
