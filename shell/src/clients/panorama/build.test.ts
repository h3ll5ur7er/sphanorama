// The build client's decisions: when a build starts, what the page says while it runs, and what
// it draws when it ends. Never the pixels themselves — `paintPreviewOnCanvas` has its own tests,
// and what is worth asserting here is *whether* it is asked to paint, and with what.
import { afterEach, describe, expect, it, vi } from 'vitest';
import type {
  BuildId, BuildProgress, BuildStage, FramePreview, Status,
} from '../../../../contracts/ts/contracts';
import { type BuildCore, type BuildElements, createBuildClient } from './build';

afterEach(() => { vi.restoreAllMocks(); });

function elements(): BuildElements {
  return {
    button: document.createElement('button'),
    status: document.createElement('p'),
    progress: document.createElement('progress'),
    canvas: document.createElement('canvas'),
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

/** A core that answers each poll with the next progress it is given, and records every call. */
function scriptedCore(polls: BuildProgress[]) {
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
  };
  return { core, calls };
}

const immediately = () => Promise.resolve();

describe('building a preview', () => {
  it('starts a build, polls it to the end, and paints what it made', async () => {
    const { core, calls } = scriptedCore([
      progress('Features', 0.25), progress('GlobalSolve', 0.75), progress('Complete', 1),
    ]);
    const ui = elements();
    const paint = vi.fn();
    const client = createBuildClient(ui, core, paint, immediately);

    await client.build();

    expect(calls).toEqual(['start', 'poll 1', 'poll 1', 'poll 1', 'preview 1']);
    expect(paint).toHaveBeenCalledWith(ui.canvas, preview);
    expect(ui.canvas.hidden).toBe(false);
    expect(ui.progress.value).toBe(1);
    expect(ui.button.disabled).toBe(false);
    expect(ui.status.textContent).toMatch(/built/i);
  });

  it('shows how far the build has got while it runs', async () => {
    let release: () => void = () => {};
    const seen: Array<[number, string]> = [];
    const { core } = scriptedCore([progress('Features', 0.25), progress('Complete', 1)]);
    const ui = elements();
    // The page yields between polls; that is where the progress has to be on screen already.
    const yieldToPage = () => {
      seen.push([ui.progress.value, ui.status.textContent ?? '']);
      return new Promise<void>((resolve) => { release = resolve; });
    };
    const client = createBuildClient(ui, core, vi.fn(), yieldToPage);

    const running = client.build();
    await vi.waitFor(() => expect(seen).toHaveLength(1));
    expect(seen[0][0]).toBe(0.25);
    expect(seen[0][1]).toMatch(/features/i);
    expect(ui.button.disabled).toBe(true);
    release();
    await running;
  });

  it('starts nothing while a build is already running', async () => {
    let release: () => void = () => {};
    const { core, calls } = scriptedCore([progress('Features', 0.5), progress('Complete', 1)]);
    const client = createBuildClient(elements(), core, vi.fn(),
      () => new Promise<void>((resolve) => { release = resolve; }));

    const first = client.build();
    await vi.waitFor(() => expect(calls).toContain('poll 1'));
    await client.build();
    release();
    await first;

    expect(calls.filter((call) => call === 'start')).toHaveLength(1);
  });

  it('says why a build was refused, and lets you try again', async () => {
    const { core, calls } = scriptedCore([]);
    core.start = async () => {
      calls.push('start');
      return {
        ok: false,
        status: { code: 'FailedPrecondition', component: 'PanoramaBuildManager',
          detail: 'no frame of this capture was taken at a measured pose' } as Status,
      };
    };
    const ui = elements();
    const paint = vi.fn();
    const client = createBuildClient(ui, core, paint, immediately);

    await client.build();

    expect(ui.status.textContent).toContain('no frame of this capture was taken at a measured pose');
    expect(paint).not.toHaveBeenCalled();
    expect(calls).toEqual(['start']);
    expect(ui.button.disabled).toBe(false);
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
    const client = createBuildClient(ui, core, vi.fn(), immediately);

    await client.build();
    expect(ui.canvas.hidden).toBe(false);
    await client.build();
    // A picture of the last capture under a heading about this one's failure would be read as
    // this one's.
    expect(ui.canvas.hidden).toBe(true);
  });
});
