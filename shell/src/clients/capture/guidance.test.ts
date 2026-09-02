import { describe, expect, it } from 'vitest';
import type { CaptureGuidance, CoverageState, NodeId } from '../../../../contracts/ts/contracts';
import {
  RETICLE_LOCKED_RADIUS, RETICLE_MAX_RADIUS, describeGuidance, reticleRadius,
} from './guidance';

const guidance = (over: Partial<CaptureGuidance> = {}): CaptureGuidance => ({
  targetNode: 7 as NodeId,
  angularErrorDeg: 12,
  rollErrorDeg: 0,
  stability: 1,
  action: 'Seek',
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
