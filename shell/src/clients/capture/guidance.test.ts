import { describe, expect, it } from 'vitest';
import type { CaptureGuidance, CoverageState, NodeId } from '../../../../contracts/ts/contracts';
import {
  RETICLE_LOCKED_RADIUS, RETICLE_MAX_RADIUS, describeGuidance, reticleRadius,
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
  it('does not close on a cell whose cone is not a measurement', () => {
    // The sixth copy of one rule, and the one the user actually looks at. `ICoveragePlannerEngine`
    // requires an acceptance cone to be finite and positive; both engines' `Plan` refuse otherwise,
    // both `Locate`s skip such a cell, and `ArmBurst` refuses it. This function is the page's own
    // reading of the same number, and it had no usability rule at all.
    //
    // What that looked like: on an `inf` or `NaN` cone `!(90 > cone)` is false, so the ring drew
    // fully closed and took the `locked` class, ninety degrees off target, while the core said
    // `Seek` and nothing ever fired. Which is the exact symptom the engine header names as the
    // reason the guards must agree — "the reticle would close on a cell that then would not arm".
    //
    // `-Infinity` was a second defect wearing the first one's clothes: the interpolation produced
    // `NaN`, `NaN.toFixed(1)` is the string "NaN", and an invalid SVG length either drops the
    // circle or freezes it at its last radius. The same failure the overlay's `holding` clamp was
    // given a guard for one file away, on the sibling number painted from the same tick.
    for (const cone of [Number.POSITIVE_INFINITY, Number.NEGATIVE_INFINITY, Number.NaN, 0, -5]) {
      const radius = reticleRadius(90, cone);
      expect(Number.isFinite(radius), `a cone of ${cone} produced a radius of ${radius}`).toBe(true);
      expect(radius, `the reticle locked onto a cell 90 degrees away, on a cone of ${cone}`)
        .toBe(RETICLE_MAX_RADIUS);
    }
  });

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
  it('parks the ring for an angle nobody measured, rather than closing it', () => {
    // The same rule as the cone guard above, applied to the other unusable number. A NaN error is
    // a sensor that reported nothing usable, and `!(NaN > cone)` is *true* — so the spelling
    // chosen to keep NaN out of the growth arithmetic sent it to `RETICLE_LOCKED_RADIUS` instead:
    // the fully-closed "you are on target" ring, drawn for a pose that does not exist. The comment
    // beside it said it "parks the ring", which is what the widest radius does and what this now
    // returns.
    //
    // Found by a reviewer noticing that the two guards two lines apart disagreed about the same
    // class of input, and that neither spelling had a test.
    expect(reticleRadius(Number.NaN, 4)).toBe(RETICLE_MAX_RADIUS);
    expect(reticleRadius(Number.POSITIVE_INFINITY, 4)).toBe(RETICLE_MAX_RADIUS);
    // And the ordinary cases still land where they did, since the guard has to be narrower than
    // "anything that is not smaller than the cone".
    expect(reticleRadius(0, 4)).toBe(RETICLE_LOCKED_RADIUS);
    expect(reticleRadius(4, 4)).toBe(RETICLE_LOCKED_RADIUS);
    expect(reticleRadius(200, 4)).toBe(RETICLE_MAX_RADIUS);
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
    // Before a session's first reading arrives the pose is the identity it was born with, so
    // `angularErrorDeg` comes back 0 for whichever cell happens to sit straight ahead. `cell 13 ·
    // 0° off` reads as "perfectly aimed" at a cell the app cannot locate — and it sits next to a
    // reticle deliberately parked wide open, so the line and the ring said opposite things. The
    // window is short now that a device with no sensor is refused outright (ADR 0044); it is not
    // empty, and a stream carrying rates with no attitude never leaves it.
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
