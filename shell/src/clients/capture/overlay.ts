/**
 * What the capture overlay draws: a ring on every planned cell in view, and an arrow toward the
 * one to go to when it is not in view.
 *
 * Decisions only — where the rings sit, how full each is, whether an arrow is needed — so that the
 * part worth being sure of can be tested without a browser. Turning this into elements is the
 * painter's job, and how any of it *looks* is reviewed by eye.
 *
 * Nothing here chooses which cell to go to. That is the planner's answer (V4), arriving as
 * `CaptureGuidance.targetNode`, and re-deriving it from distances would put a second opinion about
 * coverage in a client — which is the mistake the ranking and the coverage map were both saved
 * from earlier.
 */
import type { CapturePlan, CoverageState, NodeId, Quat } from '../../../../contracts/ts/contracts';
import { sight } from './sighting';

export interface RingMark {
  node: NodeId;
  /** Fractions of the viewfinder: x grows right, y grows down. */
  x: number;
  y: number;
  /**
   * How much of the ring is drawn, from 0 to 1.
   *
   * A fraction rather than a flag because captured-or-not is the *current* answer and not the
   * interesting one: a hold-still timer counting toward a burst reads as a filling ring, and a
   * fill that only ever took two values would have to be rebuilt to say so. Nothing drives a
   * middle value yet; `holding` is where one would arrive.
   */
  fill: number;
  /** Whether this is the cell guidance is sending the user to. */
  isTarget: boolean;
}

export interface ArrowMark {
  /** Degrees clockwise from straight up. */
  bearingDeg: number;
}

export interface Overlay {
  rings: RingMark[];
  arrow: ArrowMark | null;
}

export interface OverlayInput {
  plan: CapturePlan;
  coverage: CoverageState;
  attitude: Quat;
  targetNode: NodeId;
  /** Progress toward capturing the cell being aimed at, from 0 to 1. */
  holding?: number;
}

export function planOverlay(input: OverlayInput): Overlay {
  const { plan, coverage, attitude, targetNode } = input;
  const lens = {
    horizontalFovDeg: plan.spec?.horizontalFovDeg ?? 0,
    verticalFovDeg: plan.spec?.verticalFovDeg ?? 0,
  };
  // A plan with no lens was never sized — Begin resolves the angles from the camera — and placing
  // markers through a frustum of zero width lands them on infinity. Drawing nothing says "not yet"
  // where a ring at the edge of the screen would say something false about where a cell is.
  const sized = lens.horizontalFovDeg > 0 && lens.verticalFovDeg > 0;
  if (!sized) return { rings: [], arrow: null };

  const holes = new Set<number>(coverage.holes as unknown as number[]);
  const holding = Math.min(1, Math.max(0, input.holding ?? 0));

  const rings: RingMark[] = [];
  let arrow: ArrowMark | null = null;

  for (const node of plan.nodes) {
    const seen = sight(attitude, node.targetOrientation, lens);
    const isTarget = (node.id as unknown as number) === (targetNode as unknown as number);
    // The arrow is only ever about the cell being aimed at, and only when it cannot be seen.
    const needsArrow = isTarget && !seen.onScreen;
    if (needsArrow) arrow = { bearingDeg: seen.bearingDeg };
    if (!seen.onScreen) continue;

    // Captured wins over any hold in progress: progress toward taking something is meaningless
    // once it has been taken, and a ring that emptied itself while the user lingered would read
    // as losing the frame they had just got.
    const captured = !holes.has(node.id as unknown as number);
    const fill = captured ? 1 : isTarget ? holding : 0;
    rings.push({ node: node.id, x: seen.x, y: seen.y, fill, isTarget });
  }

  return { rings, arrow };
}
