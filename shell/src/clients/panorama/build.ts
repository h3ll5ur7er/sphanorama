/**
 * The build client: one button that turns the capture into a preview panorama, the progress while
 * it runs, and the picture when it is done (ADR 0071).
 *
 * It decides nothing about the panorama. Which frames, which pairs and which lens are the core's
 * (ADR 0070); what is here is when to ask, how often, and what to put on the page with the answer.
 * The core arrives as three functions rather than the facade, so this can be driven without a
 * worker and so the only calls it can make are the three it is meant to.
 */
import type {
  BuildId, BuildProgress, BuildStage, FramePreview, Status,
} from '../../../../contracts/ts/contracts';

/** A value, or a failure carrying the core's status — which, unlike the review strip, this shows. */
type Answered<T> = { readonly ok: true; readonly value: T }
  | { readonly ok: false; readonly status?: Status };

export interface BuildCore {
  start(): Promise<Answered<BuildId>>;
  poll(build: BuildId): Promise<Answered<BuildProgress>>;
  preview(build: BuildId): Promise<Answered<FramePreview>>;
}

export interface BuildElements {
  button: HTMLButtonElement;
  status: HTMLElement;
  progress: HTMLProgressElement;
  canvas: HTMLCanvasElement;
}

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
  Projecting: 'drawing the panorama',
};

const why = (status: Status | undefined) => status?.detail || status?.code || 'no reason given';

/**
 * Builds when asked, and says how it is going.
 *
 * `yieldToPage` runs between polls. Each poll is one step of the build on the core's thread
 * (ADR 0070), so the page is never held by one; yielding between them is what lets it paint the
 * progress and keep the viewfinder moving while a sphere's worth of steps goes by.
 */
export function createBuildClient(
  ui: BuildElements, core: BuildCore, paint: PaintPanorama,
  yieldToPage: () => Promise<void> = () => new Promise((resolve) => { setTimeout(resolve, 0); }),
): { build(): Promise<void> } {
  let running = false;
  ui.canvas.hidden = true;

  async function run(): Promise<void> {
    ui.progress.value = 0;
    ui.status.textContent = 'Building a preview: starting.';
    const started = await core.start();
    if (!started.ok) {
      ui.status.textContent = `The build was refused: ${why(started.status)}.`;
      return;
    }
    for (;;) {
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
      await yieldToPage();
    }
    const preview = await core.preview(started.value);
    if (!preview.ok) {
      ui.status.textContent = `The build finished, but its preview could not be read: ${why(preview.status)}.`;
      return;
    }
    paint(ui.canvas, preview.value);
    ui.canvas.hidden = false;
    ui.status.textContent = `Built: a ${preview.value.width} by ${preview.value.height} preview.`;
  }

  return {
    async build() {
      if (running) return;
      running = true;
      ui.button.disabled = true;
      // Out of sight until this build has something to show: last time's picture under this
      // time's progress, or this time's failure, would be read as this one's.
      ui.canvas.hidden = true;
      try {
        await run();
      } catch (reason: unknown) {
        // A rejection is a worker that went away or answered something unreadable; the screen says
        // the build stopped, and the reason goes where a developer will look.
        console.error('sphanorama build: a call to the core did not answer', reason);
        ui.status.textContent = 'The build stopped: the core did not answer.';
      } finally {
        running = false;
        ui.button.disabled = false;
      }
    },
  };
}
