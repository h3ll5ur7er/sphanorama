/**
 * The sphere as a flat map: where each planned cell sits, and which of them are still holes.
 *
 * Equirectangular, because the thing being reviewed is coverage rather than geometry — the
 * question is "have I got that one yet", and a projection that distorts area but keeps azimuth
 * and elevation as straight lines answers it at a glance. A cell's *place* is presentation and
 * belongs here; whether it counts as covered is `ICoveragePlannerEngine::Evaluate`'s answer,
 * asked through the manager, and is passed through untouched.
 */
import type { CapturePlan, CoverageState, NodeId } from '../../../../contracts/ts/contracts';
import { aimOf } from '../spherical';

export interface MappedCell {
  node: NodeId;
  /** Fraction of the map's width, 0 at the left edge. Straight ahead is the middle. */
  x: number;
  /** Fraction of the map's height, 0 at the top. Straight up is the top. */
  y: number;
  state: 'covered' | 'hole';
}

export function mapCoverage(plan: CapturePlan, coverage: CoverageState): MappedCell[] {
  const holes = new Set<number>(coverage.holes);
  return plan.nodes.map((node) => {
    const { azimuthDeg, elevationDeg } = aimOf(node.targetOrientation);
    return {
      node: node.id,
      // Ahead in the middle, so the seam falls behind the user — the part of a sphere a map is
      // least often read at, and the part a capture is least often aimed at.
      x: ((azimuthDeg + 180) % 360) / 360,
      y: (90 - elevationDeg) / 180,
      state: holes.has(node.id) ? 'hole' : 'covered',
    };
  });
}
