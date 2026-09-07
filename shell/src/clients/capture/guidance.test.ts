import { describe, expect, it } from 'vitest';
import type { CaptureGuidance, CoverageState, NodeId } from '../../../../contracts/ts/contracts';
import {
  RETICLE_LOCKED_RADIUS, RETICLE_MAX_RADIUS, canCapture, describeGuidance, reticleRadius,
  unwrapDegrees,
} from './guidance';

const guidance = (over: Partial<CaptureGuidance> = {}): CaptureGuidance => ({
  targetNode: 7 as NodeId,
  angularErrorDeg: 12,
  rollErrorDeg: 0,
  stability: 1,
  action: 'Seek',
  aimKnown: true,
  heldFraction: 0,
  ...over,
});

const coverage = (over: Partial<CoverageState> = {}): CoverageState => ({
  nodesTotal: 32,
  nodesSatisfied: 5,
  coveredSolidAngleFraction: 5 / 32,
  holes: [],
  underOverlapped: [],
  ...over,
});

describe('reticleRadius', () => {
  it('rests on the acceptance ring while the phone is aimed well enough', () => {
    // Inside the cone the frame is acceptable, so the ring must stop moving: a reticle that
    // keeps twitching invites the user to keep correcting an aim that is already good.
    expect(reticleRadius(0, 4)).toBe(RETICLE_LOCKED_RADIUS);
    expect(reticleRadius(4, 4)).toBe(RETICLE_LOCKED_RADIUS);
  });

  it('opens up as the aim gets worse, and stops opening', () => {
    const near = reticleRadius(10, 4);
    const far = reticleRadius(30, 4);
    expect(near).toBeGreaterThan(RETICLE_LOCKED_RADIUS);
    expect(far).toBeGreaterThan(near);
    // Bounded, or the ring leaves the viewBox and the user sees nothing at all when most lost.
    expect(reticleRadius(180, 4)).toBe(RETICLE_MAX_RADIUS);
  });

  it('stays on screen for a degenerate cone rather than dividing by it', () => {
    const radius = reticleRadius(1, 0);
    expect(Number.isFinite(radius)).toBe(true);
    expect(radius).toBeLessThanOrEqual(RETICLE_MAX_RADIUS);
  });
});

describe('describeGuidance', () => {
  it('names the cell and how far off the aim is while seeking', () => {
    expect(describeGuidance(guidance(), coverage())).toBe('cell 7 · 12° off · 5/32 done');
  });

  it('says to hold still once the cell is in reach', () => {
    const text = describeGuidance(guidance({ action: 'HoldStill', angularErrorDeg: 2 }), coverage());
    expect(text).toContain('hold still');
    expect(text).toContain('5/32');
  });

  it('says a cell is already captured rather than falling through to a bare angle', () => {
    // The default arm renders `cell 7 · 0° off`, which reads as "keep aiming" at a cell that needs
    // nothing — and `AlreadyCaptured` is the resting state of any phone left pointing at a
    // finished cell, so it is the line a user sees most often after a capture. Deleting the case
    // left every test green.
    const text = describeGuidance(
      guidance({ action: 'AlreadyCaptured', angularErrorDeg: 0 }), coverage());
    expect(text).toContain('already captured');
    expect(text).not.toContain('off');
  });

  it('leaves the angle out when there was no aim to measure it from', () => {
    // A phone with no motion sensor reports identity for the whole session, so `angularErrorDeg`
    // comes back 0 for whichever cell happens to sit straight ahead. `cell 13 · 0° off` reads as
    // "perfectly aimed" at a cell the app cannot locate — and it sat next to a reticle deliberately
    // parked wide open, so the line and the ring said opposite things.
    const blind = describeGuidance(
      guidance({ action: 'Seek', angularErrorDeg: 0, aimKnown: false }), coverage());
    expect(blind).not.toContain('off');
    expect(blind).toContain('cell 7');
    expect(blind).toContain('5/32');

    // And it is still there when there is an aim, because that is the number a user steers by.
    expect(describeGuidance(guidance({ action: 'Seek', angularErrorDeg: 12 }), coverage()))
      .toContain('12° off');
  });

  it('reports too-fast motion instead of an aim the user cannot act on', () => {
    // Angular error is meaningless while the phone is whipping around; telling the user to slow
    // down is the only instruction that helps.
    expect(describeGuidance(guidance({ action: 'TooFast' }), coverage())).toContain('slow down');
  });

  it('announces a finished sphere without a cell number', () => {
    const text = describeGuidance(
      guidance({ action: 'SphereDone' }), coverage({ nodesSatisfied: 32 }));
    expect(text).toContain('sphere complete');
    expect(text).not.toContain('cell');
  });
});

describe('unwrapDegrees', () => {
  it('leaves an angle alone when it has not wrapped', () => {
    expect(unwrapDegrees(0, 30)).toBe(30);
    expect(unwrapDegrees(30, -30)).toBe(-30);
  });

  it('takes the short way round the seam instead of the long way back', () => {
    // rollErrorDeg comes back in (-180, 180], so rolling the phone past the seam is a 358-degree
    // step in the number and two degrees of movement in the hand. Handed to the SVG as-is, the
    // horizon spins a full turn the wrong way inside the 80ms transition — which is a marker
    // travelling across the screen, the same complaint the rotation centre caused.
    expect(unwrapDegrees(179, -179)).toBe(181);
    expect(unwrapDegrees(-179, 179)).toBe(-181);
  });

  it('keeps counting past a full turn rather than snapping back', () => {
    // Continuity is the whole point: an angle that resets to zero every revolution would flick
    // the horizon once per turn, which is exactly what this exists to prevent.
    let shown = 0;
    for (const step of [90, 180, -90, 0, 90, 180, -90, 0]) shown = unwrapDegrees(shown, step);
    expect(shown).toBe(720);
  });

  it('is stable when nothing moved', () => {
    expect(unwrapDegrees(540, 180)).toBe(540);
  });
});

describe('canCapture', () => {
  const BLIND = false;
  const at = (action: CaptureGuidance['action'], aimKnown = true): CaptureGuidance => ({
    targetNode: 3 as NodeId,
    angularErrorDeg: 0,
    rollErrorDeg: 0,
    stability: 1,
    action,
    aimKnown,
    heldFraction: 0,
  });

  it('offers nothing at all where there is an aim, whatever guidance says', () => {
    // Since ADR 0043 the dwell fires the burst, so with an aim there is no shutter to gate. Every
    // action, including the one this used to be the whole rule for: a button beside an automatic
    // trigger is two ways to do one thing, and the one the finger reaches for moves the phone it is
    // supposed to be holding still.
    for (const action of
      ['HoldStill', 'Seek', 'AlreadyCaptured', 'Firing', 'CellDone', 'SphereDone', 'TooFast',
       'Fire'] as const) {
      expect(canCapture(at(action)), action).toBe(false);
    }
  });

  // UC-4. A phone that declined motion, or has none, reports identity for the whole session, so
  // the core never says `HoldStill` — it targets by coverage instead and says `Seek` (ADR 0042).
  // A gate written only for the aimed case offered one cell of thirty-two and then nothing.
  it('offers the cell coverage named when there is no aim to check', () => {
    expect(canCapture(at('Seek', BLIND))).toBe(true);
  });

  it('still offers nothing blind while a burst runs or the sphere is finished', () => {
    for (const action of ['Firing', 'CellDone', 'SphereDone', 'TooFast', 'Fire'] as const) {
      expect(canCapture(at(action, BLIND)), action).toBe(false);
    }
  });

  it('does not invent an aimed action blind', () => {
    // `HoldStill` cannot arrive without a measured pose, so a gate that accepted it blind would be
    // claiming the camera is inside a cone nobody measured.
    expect(canCapture(at('HoldStill', BLIND))).toBe(false);
    expect(canCapture(at('AlreadyCaptured', BLIND))).toBe(false);
  });
});
