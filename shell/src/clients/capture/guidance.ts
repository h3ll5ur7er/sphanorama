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
    default:
      return `${cell} · ${off} · ${progress}`;
  }
}
