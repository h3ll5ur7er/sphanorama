// The build client's decisions: when a build starts, what the page says while it runs, and what
// it draws when it ends. Never the pixels themselves — `paintPreviewOnCanvas` has its own tests,
// and what is worth asserting here is *whether* it is asked to paint, and with what.
import { afterEach, describe, expect, it, vi } from 'vitest';
import type {
  BuildId, BuildProgress, BuildStage, CapturePlan, CoverageGap, FramePreview, Status,
} from '../../../../contracts/ts/contracts';
import {
  type BuildCore, type BuildElements, captureNeedsCore, createBuildClient, yieldWhile,
} from './build';

afterEach(() => { vi.restoreAllMocks(); });

function elements(): BuildElements {
  return {
    button: document.createElement('button'),
    status: document.createElement('p'),
    progress: document.createElement('progress'),
    canvas: document.createElement('canvas'),
    fill: document.createElement('button'),
  };
}

const ok = { code: 'Ok', component: '', detail: '' } as Status;

function progress(stage: BuildStage, fraction: number, failure: Status = ok): BuildProgress {
  return { id: 1 as BuildId, stage, fraction, tilesReady: 0, tilesTotal: 0, failure };
}

const preview: FramePreview = {
  frame: 9 as FramePreview['frame'], width: 4, height: 2, format: 'RGBA8',
  pixels: new Uint8Array(4 * 2 * 4),
} as FramePreview;

/** A gap of this share of the sphere; where it is does not reach the page. */
function gap(solidAngleFraction: number): CoverageGap {
  return { target: { w: 1, x: 0, y: 0, z: 0 }, solidAngleFraction } as CoverageGap;
}

/**
 * A core that answers each poll with the next progress it is given, a sphere with nothing missing
 * unless told otherwise, and records every call.
 */
function scriptedCore(polls: BuildProgress[], gaps: CoverageGap[] = []) {
  const calls: string[] = [];
  const core: BuildCore = {
    start: async () => { calls.push('start'); return { ok: true, value: 1 as BuildId }; },
    poll: async (build) => {
      calls.push(`poll ${build}`);
      const next = polls.shift();
      if (next === undefined) throw new Error('polled past the end of the script');
      return { ok: true, value: next };
    },
    preview: async (build) => { calls.push(`preview ${build}`); return { ok: true, value: preview }; },
    gaps: async (build) => { calls.push(`gaps ${build}`); return { ok: true, value: gaps }; },
    fill: async (asked) => {
      calls.push(`fill ${asked.length}`);
      return { ok: true, value: { nodes: [], spec: {} } as unknown as CapturePlan };
    },
  };
  return { core, calls };
}

const immediately = () => Promise.resolve();

/** A painter that draws, as `paintPreviewOnCanvas` does: it marks the canvas it drew on. */
const drawing = () => vi.fn((canvas: HTMLCanvasElement) => { canvas.dataset.preview = 'ready'; });

describe('building a preview', () => {
  it('shows nothing when the preview could not be drawn, rather than the last one', async () => {
    // The painter refuses a preview it cannot draw by marking the canvas and leaving its pixels
    // alone — so the canvas still holds the last build's picture, and showing it would put that
    // picture under this build's "built".
    const { core } = scriptedCore([progress('Complete', 1), progress('Complete', 1)]);
    const ui = elements();
    let refuse = false;
    const paint = vi.fn((canvas: HTMLCanvasElement) => {
      canvas.dataset.preview = refuse ? 'malformed' : 'ready';
    });
    const client = createBuildClient(ui, core, paint, immediately);

    await client.build();
    expect(ui.canvas.hidden).toBe(false);
    refuse = true;
    await client.build();

    expect(ui.canvas.hidden).toBe(true);
    expect(ui.status.textContent).toMatch(/could not be drawn/i);
  });

  it('does not take a mark the last build left for this one\'s', async () => {
    const { core } = scriptedCore([progress('Complete', 1), progress('Complete', 1)]);
    const ui = elements();
    const paint = drawing();
    const client = createBuildClient(ui, core, paint, immediately);
    await client.build();
    // A painter that returns without marking anything, after one that marked the canvas ready.
    paint.mockImplementation(() => {});

    await client.build();

    expect(ui.canvas.hidden).toBe(true);
  });

  it('starts a build, polls it to the end, and paints what it made', async () => {
    const { core, calls } = scriptedCore([
      progress('Features', 0.25), progress('GlobalSolve', 0.75), progress('Complete', 1),
    ]);
    const ui = elements();
    const paint = drawing();
    const client = createBuildClient(ui, core, paint, immediately);

    await client.build();

    expect(calls).toEqual(['start', 'poll 1', 'poll 1', 'poll 1', 'preview 1', 'gaps 1']);
    expect(paint).toHaveBeenCalledWith(ui.canvas, preview);
    expect(ui.canvas.hidden).toBe(false);
    expect(ui.progress.value).toBe(1);
    expect(ui.button.disabled).toBe(false);
    expect(ui.status.textContent).toMatch(/built/i);
  });

  it('waits for the page before every call it makes to the core', async () => {
    // A burst needs the core on consecutive ticks, and a press can come in the middle of one: the
    // start and the preview are calls on the core's thread as much as a poll is.
    // Each wait ends on a later task, and says so: a call made before it ended — a wait started
    // and not awaited — lands between the two.
    const { core, calls } = scriptedCore([progress('Features', 0.5), progress('Complete', 1)]);
    const client = createBuildClient(elements(), core, drawing(), async () => {
      calls.push('wait');
      await new Promise((resolve) => { setTimeout(resolve, 0); });
      calls.push('waited');
    });

    await client.build();

    expect(calls).toEqual([
      'wait', 'waited', 'start',
      'wait', 'waited', 'poll 1',
      'wait', 'waited', 'poll 1',
      'wait', 'waited', 'preview 1',
      'wait', 'waited', 'gaps 1',
    ]);
  });

  it('names each stage the build reports', async () => {
    const said: string[] = [];
    const { core } = scriptedCore([
      progress('Queued', 0), progress('Features', 0.2), progress('PairwiseMatching', 0.4),
      progress('GlobalSolve', 0.6), progress('CoverageCheck', 0.7), progress('Projecting', 0.8),
      progress('Complete', 1),
    ]);
    const ui = elements();
    const client = createBuildClient(ui, core, drawing(), async () => {
      said.push(ui.status.textContent ?? '');
    });

    await client.build();

    expect(said).toEqual([
      'Building a preview: starting.',
      'Building a preview: starting.',
      'Building a preview: starting.',
      'Building a preview: finding features in each frame.',
      'Building a preview: matching neighbouring frames.',
      'Building a preview: solving for where each frame points.',
      'Building a preview: checking the sphere for gaps.',
      'Building a preview: drawing the panorama.',
      // And the wait before the preview is read, which says the same until it is drawn, and the
      // one before its gaps are, which says it is drawn.
      'Building a preview: drawing the panorama.',
      'Built: a 4 by 2 preview.',
    ]);
    expect(ui.status.textContent)
      .toBe('Built: a 4 by 2 preview. Nothing the capture planned is missing.');
  });

  it('shows how far the build has got while it runs', async () => {
    let release: () => void = () => {};
    const seen: Array<[number, string]> = [];
    const { core } = scriptedCore([progress('Features', 0.25), progress('Complete', 1)]);
    const ui = elements();
    // The page yields between polls; that is where the progress has to be on screen already. Held
    // at the one after the first step, so the button can be seen disabled mid-build.
    const yieldToPage = () => {
      if (ui.progress.value === 0 || seen.length > 0) return Promise.resolve();
      seen.push([ui.progress.value, ui.status.textContent ?? '']);
      return new Promise<void>((resolve) => { release = resolve; });
    };
    const client = createBuildClient(ui, core, vi.fn(), yieldToPage);

    const running = client.build();
    await vi.waitFor(() => expect(seen).toHaveLength(1));
    expect(seen[0][0]).toBe(0.25);
    expect(seen[0][1]).toContain('finding features in each frame');
    expect(ui.button.disabled).toBe(true);
    release();
    await running;
  });

  it('starts nothing while a build is already running', async () => {
    let release: () => void = () => {};
    const { core, calls } = scriptedCore([progress('Features', 0.5), progress('Complete', 1)]);
    // Held once, after the build has polled, so the second press lands on a build that is running.
    let held = false;
    const client = createBuildClient(elements(), core, vi.fn(), () => {
      if (held || !calls.includes('poll 1')) return Promise.resolve();
      held = true;
      return new Promise<void>((resolve) => { release = resolve; });
    });

    const first = client.build();
    await vi.waitFor(() => expect(calls).toContain('poll 1'));
    await client.build();
    release();
    await first;

    expect(calls.filter((call) => call === 'start')).toHaveLength(1);
  });

  it('starts nothing while the first press is still waiting for the capture', async () => {
    // The wait before the start can last a whole burst, and nothing has reached the core yet.
    let release: () => void = () => {};
    const { core, calls } = scriptedCore([progress('Complete', 1)]);
    let held = false;
    const client = createBuildClient(elements(), core, vi.fn(), () => {
      if (held) return Promise.resolve();
      held = true;
      return new Promise<void>((resolve) => { release = resolve; });
    });

    const first = client.build();
    await client.build();
    release();
    await first;

    expect(calls.filter((call) => call === 'start')).toHaveLength(1);
  });

  it('says why a build was refused, and lets you try again', async () => {
    const { core, calls } = scriptedCore([progress('Complete', 1)]);
    const started = core.start;
    let refuse = true;
    core.start = async () => {
      if (!refuse) return started();
      calls.push('start');
      return {
        ok: false,
        status: { code: 'FailedPrecondition', component: 'PanoramaBuildManager',
          detail: 'no frame of this capture was taken at a measured pose' } as Status,
      };
    };
    const ui = elements();
    const paint = drawing();
    const client = createBuildClient(ui, core, paint, immediately);

    await client.build();

    expect(ui.status.textContent).toContain('no frame of this capture was taken at a measured pose');
    expect(paint).not.toHaveBeenCalled();
    expect(calls).toEqual(['start']);
    expect(ui.button.disabled).toBe(false);

    refuse = false;
    await client.build();

    expect(calls).toEqual(['start', 'start', 'poll 1', 'preview 1', 'gaps 1']);
    expect(ui.status.textContent).toMatch(/built/i);
    expect(ui.canvas.hidden).toBe(false);
  });

  it('says why a poll was refused, and asks nothing more of that build', async () => {
    // A build the core no longer holds — a later start took its one slot — answers `NotFound`;
    // asking again would ask forever.
    const { core, calls } = scriptedCore([progress('Features', 0.25)]);
    const polled = core.poll;
    core.poll = async (build) => {
      if (calls.includes(`poll ${build}`)) {
        calls.push(`poll ${build}`);
        // Asked a third time, the build is being retried: end it here, or the test spins forever.
        if (calls.length > 3) throw new Error('a refused poll was asked again');
        return { ok: false, status: { code: 'NotFound', component: 'PanoramaBuildManager',
          detail: 'no such build' } as Status };
      }
      return polled(build);
    };
    const ui = elements();
    const paint = vi.fn();
    const client = createBuildClient(ui, core, paint, immediately);

    await client.build();

    expect(calls).toEqual(['start', 'poll 1', 'poll 1']);
    expect(ui.status.textContent).toContain('no such build');
    expect(paint).not.toHaveBeenCalled();
    expect(ui.canvas.hidden).toBe(true);
    expect(ui.button.disabled).toBe(false);
  });

  it('says why a finished build\'s preview was refused, and shows nothing', async () => {
    const { core, calls } = scriptedCore([progress('Complete', 1)]);
    core.preview = async (build) => {
      calls.push(`preview ${build}`);
      return { ok: false, status: { code: 'FailedPrecondition', component: 'PanoramaBuildManager',
        detail: 'the store no longer holds this build\'s panorama; build again' } as Status };
    };
    const ui = elements();
    const paint = vi.fn();
    const client = createBuildClient(ui, core, paint, immediately);

    await client.build();

    expect(calls).toEqual(['start', 'poll 1', 'preview 1']);
    expect(ui.status.textContent).toContain('no longer holds this build\'s panorama');
    expect(ui.status.textContent).not.toMatch(/^built/i);
    expect(paint).not.toHaveBeenCalled();
    expect(ui.canvas.hidden).toBe(true);
    expect(ui.button.disabled).toBe(false);
  });

  it('says how much of the sphere is open, and how many frames would complete it', async () => {
    // Three targets of one hole read as three frames, which is what they are, rather than as three
    // holes.
    const { core } = scriptedCore([progress('Complete', 1)], [gap(0.012), gap(0.02), gap(0)]);
    const ui = elements();
    await createBuildClient(ui, core, drawing(), immediately).build();
    expect(ui.status.textContent).toBe('Built: a 4 by 2 preview. About 3.2% of the sphere is open;'
      + ' 3 more frames would complete the capture\'s plan.');
    expect(ui.canvas.hidden).toBe(false);
  });

  it('names one frame as one, and a share too small to round as under a tenth', async () => {
    const { core } = scriptedCore([progress('Complete', 1)], [gap(0.0004)]);
    const ui = elements();
    await createBuildClient(ui, core, drawing(), immediately).build();
    expect(ui.status.textContent).toBe('Built: a 4 by 2 preview. Under 0.1% of the sphere is open;'
      + ' 1 more frame would complete the capture\'s plan.');
  });

  it('says why the gaps could not be judged, and keeps the preview it drew', async () => {
    // The panorama does not depend on the judgement, so a refused one is said and the picture
    // stays.
    const { core, calls } = scriptedCore([progress('Complete', 1)]);
    core.gaps = async (build) => {
      calls.push(`gaps ${build}`);
      return { ok: false, status: { code: 'InvalidArgument', component: 'RingsCoveragePlanner',
        detail: 'a fill lens has to reach farther than the directions are apart' } as Status };
    };
    const ui = elements();
    await createBuildClient(ui, core, drawing(), immediately).build();
    expect(calls).toEqual(['start', 'poll 1', 'preview 1', 'gaps 1']);
    expect(ui.status.textContent).toBe('Built: a 4 by 2 preview. Whether the sphere has gaps could'
      + ' not be judged: a fill lens has to reach farther than the directions are apart.');
    expect(ui.canvas.hidden).toBe(false);
  });

  it('keeps the preview it drew when the gaps call does not answer at all', async () => {
    // The first call after the paint: a worker that goes away here takes only the gaps sentence
    // with it, not a build whose panorama is on screen.
    const error = vi.spyOn(console, 'error').mockImplementation(() => {});
    const { core } = scriptedCore([progress('Complete', 1)]);
    core.gaps = async () => { throw new Error('the worker went away'); };
    const ui = elements();
    await createBuildClient(ui, core, drawing(), immediately).build();
    expect(ui.status.textContent).toBe('Built: a 4 by 2 preview. Whether the sphere has gaps could'
      + ' not be judged: the core did not answer.');
    expect(ui.canvas.hidden).toBe(false);
    expect(ui.button.disabled).toBe(false);
    expect(error).toHaveBeenCalled();
  });

  it('hides the canvas from the start, whatever the markup said', () => {
    const ui = elements();
    ui.canvas.hidden = false;

    createBuildClient(ui, scriptedCore([]).core, vi.fn(), immediately);

    expect(ui.canvas.hidden).toBe(true);
  });

  it('says why a build failed, and draws nothing', async () => {
    const failure = {
      code: 'Unsupported', component: 'NullRegistrationEngine', detail: 'no registration here',
    } as Status;
    const { core, calls } = scriptedCore([progress('Features', 0.2), progress('Failed', 0.2, failure)]);
    const ui = elements();
    const paint = vi.fn();
    const client = createBuildClient(ui, core, paint, immediately);

    await client.build();

    expect(ui.status.textContent).toContain('no registration here');
    expect(paint).not.toHaveBeenCalled();
    expect(calls).not.toContain('preview 1');
    expect(ui.canvas.hidden).toBe(true);
    expect(ui.button.disabled).toBe(false);
  });

  it('says so when the core stops answering, rather than spinning', async () => {
    const logged = vi.spyOn(console, 'error').mockImplementation(() => {});
    const { core } = scriptedCore([]);
    core.poll = () => Promise.reject(new Error('the worker is gone'));
    const ui = elements();
    const client = createBuildClient(ui, core, vi.fn(), immediately);

    await client.build();

    expect(ui.status.textContent).toMatch(/did not answer/i);
    expect(logged).toHaveBeenCalled();
    expect(ui.button.disabled).toBe(false);
  });

  it('keeps the last preview out of sight while a new build runs', async () => {
    const { core } = scriptedCore([progress('Complete', 1), progress('Features', 0.5),
      progress('Failed', 0.5, { code: 'Internal', component: 'x', detail: 'broke' } as Status)]);
    const ui = elements();
    const client = createBuildClient(ui, core, drawing(), immediately);

    await client.build();
    expect(ui.canvas.hidden).toBe(false);
    await client.build();
    // A picture of the last capture under a heading about this one's failure would be read as
    // this one's.
    expect(ui.canvas.hidden).toBe(true);
  });
});

describe('waiting between polls', () => {
  it('waits while the capture is busy, however long, and goes on once it is not', async () => {
    // A burst advances one frame per capture tick, and a build step holds the core's one thread:
    // polling while a burst runs spreads the burst's frames across build steps, its locks held.
    // Ten minutes, so a wait that gives up — or that read `busy` once and slept — is told apart
    // from one that asks until the answer changes.
    vi.useFakeTimers();
    try {
      let busy = true;
      let resolved = false;
      void yieldWhile(() => busy, 50)().then(() => { resolved = true; });
      await vi.advanceTimersByTimeAsync(10 * 60 * 1000);
      expect(resolved).toBe(false);
      busy = false;
      await vi.advanceTimersByTimeAsync(50);
      expect(resolved).toBe(true);
    } finally {
      vi.useRealTimers();
    }
  });

  it('yields once even when nothing is busy, so the page can paint', async () => {
    vi.useFakeTimers();
    try {
      let resolved = false;
      void yieldWhile(() => false, 50)().then(() => { resolved = true; });
      // A page paints between tasks, never between microtasks: a yield that a run of microtasks
      // can see through has not let it paint.
      for (let i = 0; i < 20; i += 1) await Promise.resolve();
      expect(resolved).toBe(false);
      await vi.advanceTimersByTimeAsync(0);
      expect(resolved).toBe(true);
    } finally {
      vi.useRealTimers();
    }
  });
});

describe('whether the capture needs the core', () => {
  const idle = { stopped: false, arming: false, armed: false, firing: false };

  it('does while a burst is arming, armed or firing', () => {
    expect(captureNeedsCore({ ...idle, arming: true })).toBe(true);
    expect(captureNeedsCore({ ...idle, armed: true })).toBe(true);
    expect(captureNeedsCore({ ...idle, firing: true })).toBe(true);
  });

  it('does not when no burst is under way', () => {
    expect(captureNeedsCore(idle)).toBe(false);
  });

  it('does not once the capture has stopped, whatever its last tick left', () => {
    // A stopped capture has no next tick to finish a burst with, so a flag it left set would hold
    // a build for the life of the tab — a camera taken away mid-burst leaves `firing` true.
    expect(captureNeedsCore({ stopped: true, arming: true, armed: true, firing: true })).toBe(false);
  });
});

describe('filling the gaps a build found', () => {
  it('offers to fill only when the build found gaps', async () => {
    for (const [found, offered] of [[[gap(0.01), gap(0.02)], true], [[], false]] as const) {
      const { core } = scriptedCore([progress('Complete', 1)], [...found]);
      const ui = elements();
      ui.fill.hidden = !offered;
      await createBuildClient(ui, core, drawing(), immediately).build();
      expect(ui.fill.hidden).toBe(!offered);
    }
    // Nor when the gaps could not be judged: there is nothing to fill with.
    const { core } = scriptedCore([progress('Complete', 1)]);
    core.gaps = async () => ({ ok: false, status: { detail: 'no' } as Status });
    const ui = elements();
    await createBuildClient(ui, core, drawing(), immediately).build();
    expect(ui.fill.hidden).toBe(true);
  });

  it('fills the gaps it found, and hands the plan that results on', async () => {
    const { core, calls } = scriptedCore([progress('Complete', 1)], [gap(0.01), gap(0.02)]);
    const ui = elements();
    const filled = vi.fn();
    const waits: string[] = [];
    // Each wait records the call before it, so the last says what the fill's own wait came after.
    const client = createBuildClient(ui, core, drawing(), async () => {
      waits.push(calls.at(-1) ?? '');
    }, filled);
    // The core's answer and no other: a plan made up here, an empty one say, would pass a check
    // that `filled` was called, and the page would take it as the plan.
    const grown = { nodes: [{ id: 7 }], spec: {} } as unknown as CapturePlan;
    const asking = core.fill;
    core.fill = async (asked) => { await asking(asked); return { ok: true, value: grown }; };
    await client.build();
    await client.fill();
    expect(calls.at(-1)).toBe('fill 2');
    expect(waits.at(-1)).toBe('gaps 1');
    expect(filled).toHaveBeenCalledTimes(1);
    expect(filled.mock.calls[0][0]).toBe(grown);
    expect(ui.status.textContent).toBe(
      'Added 2 cells to the capture\'s plan: follow the guide to them, then build again.');
    expect(ui.fill.hidden).toBe(true);
    expect(ui.button.disabled).toBe(false);
  });

  it('says why a fill was refused, and leaves it to press again', async () => {
    const { core, calls } = scriptedCore([progress('Complete', 1)], [gap(0.01)]);
    const succeed = core.fill;
    core.fill = async () => ({ ok: false, status: { code: 'FailedPrecondition',
      detail: 'a burst is under way; fill once it is done' } as Status });
    const ui = elements();
    const filled = vi.fn();
    const client = createBuildClient(ui, core, drawing(), immediately, filled);
    await client.build();
    await client.fill();
    expect(ui.status.textContent).toBe(
      'The gaps could not be added to the plan: a burst is under way; fill once it is done.');
    expect(filled).not.toHaveBeenCalled();
    expect(ui.fill.hidden).toBe(false);
    expect(ui.fill.disabled).toBe(false);
    // And a press does ask again, with the gaps the refusal left in place.
    core.fill = succeed;
    await client.fill();
    expect(calls.at(-1)).toBe('fill 1');
    expect(filled).toHaveBeenCalledTimes(1);
    expect(ui.status.textContent).toBe(
      'Added 1 cell to the capture\'s plan: follow the guide to them, then build again.');
  });

  it('says so when the fill call does not answer, and leaves it to press again', async () => {
    vi.spyOn(console, 'error').mockImplementation(() => {});
    const { core, calls } = scriptedCore([progress('Complete', 1)], [gap(0.01)]);
    const succeed = core.fill;
    core.fill = async () => { throw new Error('the worker went away'); };
    const ui = elements();
    const client = createBuildClient(ui, core, drawing(), immediately);
    await client.build();
    await client.fill();
    expect(ui.status.textContent)
      .toBe('The gaps could not be added to the plan: the core did not answer.');
    expect(ui.fill.hidden).toBe(false);
    expect(ui.button.disabled).toBe(false);
    core.fill = succeed;
    await client.fill();
    expect(calls.at(-1)).toBe('fill 1');
    expect(ui.fill.hidden).toBe(true);
  });

  it('withdraws the offer when a new build starts, whose gaps it may not find', async () => {
    const { core } = scriptedCore(
      [progress('Complete', 1), progress('Features', 0.5), progress('Complete', 1)], [gap(0.01)]);
    const ui = elements();
    let shownDuring: boolean | null = null;
    let polls = 0;
    const client = createBuildClient(ui, core, drawing(), async () => {
      polls += 1;
      if (polls === 5) shownDuring = !ui.fill.hidden;
    });
    await client.build();
    expect(ui.fill.hidden).toBe(false);
    await client.build();
    expect(shownDuring).toBe(false);
  });

  it('fills once however often it is pressed, and builds nothing meanwhile', async () => {
    const { core, calls } = scriptedCore([progress('Complete', 1)], [gap(0.01)]);
    let release: () => void = () => {};
    const ui = elements();
    const client = createBuildClient(ui, core, drawing(), immediately);
    await client.build();
    core.fill = async (asked) => {
      calls.push(`fill ${asked.length}`);
      await new Promise<void>((resolve) => { release = resolve; });
      return { ok: true, value: { nodes: [], spec: {} } as unknown as CapturePlan };
    };
    const first = client.fill();
    await Promise.resolve();
    void client.fill();
    void client.build();
    expect(ui.button.disabled).toBe(true);
    expect(ui.fill.disabled).toBe(true);
    release();
    await first;
    expect(calls.filter((c) => c.startsWith('fill'))).toEqual(['fill 1']);
    expect(calls.filter((c) => c === 'start')).toEqual(['start']);
  });

  it('offers to fill only while the capture can act on it', async () => {
    // A capture with cells still to shoot is led to them already, and one that has stopped has no
    // guide to lead anywhere: either way a press would be refused or answered with nothing to do.
    const { core, calls } = scriptedCore([progress('Complete', 1), progress('Complete', 1)],
      [gap(0.01)]);
    const ui = elements();
    let can = false;
    const client = createBuildClient(ui, core, drawing(), immediately, () => {}, () => can);
    await client.build();
    expect(ui.fill.hidden).toBe(true);
    await client.fill();
    expect(calls.some((c) => c.startsWith('fill'))).toBe(false);
    can = true;
    await client.build();
    expect(ui.fill.hidden).toBe(false);
    // And asked again at the press: a loop that stopped since the build withdraws the offer.
    can = false;
    await client.fill();
    expect(calls.some((c) => c.startsWith('fill'))).toBe(false);
    expect(ui.fill.hidden).toBe(true);
  });

  it('offers nothing from a build started before the capture could act on it', async () => {
    // The gaps are judged against the capture `start` read, so a cell shot while the build ran is
    // a gap in them: offered because the capture is whole by the answer, it would send the guide
    // back to the direction just shot. Asked as the build starts, then, and again at the answer.
    const { core } = scriptedCore([progress('Features', 0.5), progress('Complete', 1)],
      [gap(0.01)]);
    const ui = elements();
    let can = false;
    const shootTheLastCell = core.poll;
    core.poll = async (build) => { can = true; return shootTheLastCell(build); };
    const client = createBuildClient(ui, core, drawing(), immediately, () => {}, () => can);
    await client.build();
    expect(can).toBe(true);
    expect(ui.fill.hidden).toBe(true);
  });

  it('says how to reach the new cells when the capture stopped during the fill', async () => {
    // The plan did grow and is written down, so it is still handed on; only the guide is gone.
    const { core } = scriptedCore([progress('Complete', 1)], [gap(0.01)]);
    const ui = elements();
    let can = true;
    const filled = vi.fn();
    const asking = core.fill;
    core.fill = async (asked) => {
      const answer = await asking(asked);
      can = false;
      return answer;
    };
    const client = createBuildClient(ui, core, drawing(), immediately, filled, () => can);
    await client.build();
    await client.fill();
    expect(filled).toHaveBeenCalledTimes(1);
    expect(ui.status.textContent).toBe(
      'Added 1 cell to the capture\'s plan: reload to be guided to them, then build again.');
  });
});
