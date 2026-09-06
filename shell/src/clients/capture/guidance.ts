/**
 * Rendering what CaptureSessionManager said, and nothing more.
 *
 * The client is not allowed to decide where a reticle belongs, how close is close enough, or
 * whether a cell is done — those are V4 and V1 decisions behind contracts. What is left is
 * presentation: turning a target cell, an angular error and an action into a ring radius and a
 * line of text.
 */
import type { CaptureGuidance, CoverageState } from '../../../../contracts/ts/contracts';

/** The fixed circle in the SVG. At or inside the acceptance cone the ring sits exactly on it. */
export const RETICLE_LOCKED_RADIUS = 7;
/** Bounded by the 100-unit viewBox, so a lost user still sees a ring rather than nothing. */
export const RETICLE_MAX_RADIUS = 44;

/** Where the ring stops growing: past this the aim is simply "somewhere else". */
const FULL_ERROR_DEG = 60;

export function reticleRadius(angularErrorDeg: number, acceptanceConeDeg: number): number {
  // `!(x > y)` rather than `<=` so a NaN error — a sensor that reported nothing usable — parks
  // the ring instead of erasing it.
  if (!(angularErrorDeg > acceptanceConeDeg)) return RETICLE_LOCKED_RADIUS;
  const span = Math.max(FULL_ERROR_DEG - acceptanceConeDeg, 1);
  const travel = Math.min(1, (angularErrorDeg - acceptanceConeDeg) / span);
  return RETICLE_LOCKED_RADIUS + (RETICLE_MAX_RADIUS - RETICLE_LOCKED_RADIUS) * travel;
}

/**
 * The nearest representative of `target` to `previous`, so a wrapping angle moves continuously.
 *
 * `rollErrorDeg` comes back in (-180, 180], and a phone rolled across that seam changes it by 358
 * degrees while moving two. Fed straight to the SVG it spins the horizon a full turn the wrong
 * way, inside a transition built for the small steps either side of the seam. Accumulating
 * instead means the number the client draws grows past a revolution and never jumps.
 */
export function unwrapDegrees(previous: number, target: number): number {
  // The positive remainder, because the accumulated angle is unbounded and JavaScript's % keeps
  // the sign of its left operand — after a few turns the plain form picks the long way round.
  const delta = ((((target - previous + 180) % 360) + 360) % 360) - 180;
  return previous + delta;
}

export function describeGuidance(guidance: CaptureGuidance, coverage: CoverageState): string {
  const progress = `${coverage.nodesSatisfied}/${coverage.nodesTotal} done`;
  const cell = `cell ${guidance.targetNode}`;
  const off = `${guidance.angularErrorDeg.toFixed(0)}° off`;

  switch (guidance.action) {
    case 'SphereDone':
      return `sphere complete · ${progress}`;
    case 'TooFast':
      return `slow down · ${progress}`;
    case 'HoldStill':
      return `${cell} · hold still · ${progress}`;
    case 'Firing':
      return `${cell} · capturing · ${progress}`;
    case 'CellDone':
      return `${cell} · captured · ${progress}`;
    // Resting on a cell that is already shot. Worded so it does not read as an instruction: the
    // dwell will not fire on a covered cell, so nothing here is asking for a second burst, and
    // re-shooting one is the retake flow's business rather than something to hint at from a
    // status line.
    case 'AlreadyCaptured':
      return `${cell} · already captured · ${progress}`;
    // The tick a dwell completed (ADR 0043). An announcement rather than an instruction — the arm
    // goes out on this tick, so by the time anyone reads it the burst is being asked for.
    //
    // Being *asked for*, not started, and the difference became reachable when the dwell learned
    // to retry: an arm can be refused, and then this line has said "capturing" about a burst that
    // never began.
    //
    // How long it says it is not one tick, which a first version of this comment claimed. `Fire`
    // goes through `sayForAWhile` in the page, so the line is held for 1200 ms, and what actually
    // replaces it is the arm's own refusal — up to the three seconds a lock write is allowed. So
    // the honest description is a sentence that is briefly ahead of itself and is corrected by the
    // failure rather than by the next frame. Kept anyway: the alternative is a word for "about
    // to, probably", which is worse to read and no truer.
    case 'Fire':
      return `${cell} · capturing · ${progress}`;
    default:
      // The angle only means something when the orientation it was measured from does. With no aim
      // it is computed against an unmeasured identity — it comes back `0° off` for whichever cell
      // sits straight ahead — and printing it beside a deliberately parked reticle told the user
      // they were perfectly aimed at a cell the app cannot locate.
      return guidance.aimKnown ? `${cell} · ${off} · ${progress}` : `${cell} · ${progress}`;
  }
}
