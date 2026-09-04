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
import { intoViewfinder, sight, type ViewfinderFit } from './sighting';

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
  /**
   * How far the phone has to turn to face the cell.
   *
   * The direction alone was not enough to aim by: a cell 20 degrees to the right and one 160
   * degrees to the right raise the same arrow, and the user turns with nothing on screen saying
   * how much is left. Reported from a phone after a full sphere.
   */
  awayDeg: number;
  /** Where to draw it, as fractions of the viewfinder — out toward the edge it points to. */
  x: number;
  y: number;
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
  /**
   * The camera frame and the box the page paints it into, when the page has measured them.
   *
   * Absent means "draw against the frame itself", which is right for a test and wrong for a
   * phone: `object-fit: cover` crops the frame to fill the box, so a marker drawn at its fraction
   * of the frame lands short of the thing it names.
   */
  fit?: ViewfinderFit;
}

/**
 * How far out the arrow sits, as a fraction from the centre.
 *
 * Out at the edge rather than in the middle, because an arrow in the centre of the picture is a
 * compass and one out at the edge is a signpost — the second reads as "over there" without being
 * decoded. Short of the very edge so the whole glyph and its number stay on screen.
 */
const ARROW_REACH = 0.4;

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

  const holes = new Set<number>(coverage.holes);
  const holding = Math.min(1, Math.max(0, input.holding ?? 0));
  const fit = input.fit;
  const place = (x: number, y: number) => (fit ? intoViewfinder(x, y, fit) : { x, y });

  const rings: RingMark[] = [];
  let arrow: ArrowMark | null = null;

  for (const node of plan.nodes) {
    const seen = sight(attitude, node.targetOrientation, lens);
    const isTarget = node.id === targetNode;
    // Captured wins over any hold in progress: progress toward taking something is meaningless
    // once it has been taken, and a ring that emptied itself while the user lingered would read
    // as losing the frame they had just got.
    const captured = !holes.has(node.id);

    // The arrow is only ever about the cell being aimed at, only when it cannot be seen, and only
    // while there is anything left to do there. Guidance goes on naming a nearest node once the
    // sphere is covered — the right answer to a different question — and a finished sphere was
    // still sending the user off across the room to a cell it had already taken.
    if (isTarget && !seen.onScreen && !captured) {
      const radians = (seen.bearingDeg * Math.PI) / 180;
      arrow = {
        bearingDeg: seen.bearingDeg,
        awayDeg: seen.awayDeg,
        // Not put through the fit, unlike a ring. A ring names a point in the camera frame and
        // has to follow the crop to stay on it; the arrow names no point at all — it is a signpost
        // at a chosen distance from the centre of the *screen*, and cropping that would march it
        // off the edge on whichever axis the fit trimmed.
        x: 0.5 + ARROW_REACH * Math.sin(radians),
        y: 0.5 - ARROW_REACH * Math.cos(radians),
      };
    }
    if (!seen.onScreen) continue;

    const fill = captured ? 1 : isTarget ? holding : 0;
    rings.push({ node: node.id, ...place(seen.x, seen.y), fill, isTarget });
  }

  return { rings, arrow };
}
