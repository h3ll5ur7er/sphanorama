// The review client's half of the sphere: where each cell sits on a flat map, and which of them
// still need a frame. Positions and states are decisions; the pixels that end up drawn there are
// reviewed by eye and are not what these assert.
import { describe, expect, it } from 'vitest';

import { mapCoverage } from './coverage-map';
import type {
  CapturePlan, CoverageNode, CoverageState, NodeId,
} from '../../../../contracts/ts/contracts';

/** A cell aimed by azimuth and elevation, built the way the core's FromAzimuthElevation builds. */
function cell(id: NodeId, azimuthDeg: number, elevationDeg: number): CoverageNode {
  const az = (azimuthDeg * Math.PI) / 360;
  const el = (elevationDeg * Math.PI) / 360;
  // Yaw about +Y then pitch about +X, which is the composition the core plans in. Expanded for
  // this pair rather than multiplied generally: with two axis rotations most of the product is
  // zero, and writing the four surviving terms is clearer than a helper that hides them.
  const cosAz = Math.cos(az), sinAz = Math.sin(az);
  const cosEl = Math.cos(el), sinEl = Math.sin(el);
  return {
    id,
    targetOrientation: {
      w: cosAz * cosEl,
      x: cosAz * sinEl,
      y: sinAz * cosEl,
      z: -sinAz * sinEl,
    },
    acceptanceConeDeg: 5,
    ringIndex: 0,
  };
}

function plan(...nodes: CoverageNode[]): CapturePlan {
  return { nodes } as CapturePlan;
}

function coverage(...holes: number[]): CoverageState {
  return {
    nodesTotal: 0, nodesSatisfied: 0, coveredSolidAngleFraction: 0,
    holes: holes as NodeId[], underOverlapped: [],
  };
}

const nothingCovered = coverage();

describe('mapCoverage', () => {
  it('puts the cell straight ahead in the middle of the map', () => {
    // Ahead is the centre rather than the left edge, so the seam falls behind the user where a
    // sphere is least often aimed and a map is least often read.
    const [ahead] = mapCoverage(plan(cell(1 as NodeId, 0, 0)), nothingCovered);
    expect(ahead.x).toBeCloseTo(0.5, 6);
    expect(ahead.y).toBeCloseTo(0.5, 6);
  });

  it('puts up at the top and down at the bottom', () => {
    // A map with the sky at the bottom is not wrong so much as unreadable, and nothing else in
    // the client would catch it.
    const [up, down] = mapCoverage(plan(cell(1 as NodeId, 0, 90), cell(2 as NodeId, 0, -90)), nothingCovered);
    expect(up.y).toBeCloseTo(0, 6);
    expect(down.y).toBeCloseTo(1, 6);
  });

  it('spreads a full turn across the whole width', () => {
    const cells = mapCoverage(
      plan(cell(1 as NodeId, -90, 0), cell(2 as NodeId, 0, 0), cell(3 as NodeId, 90, 0)), nothingCovered);
    expect(cells[0].x).toBeCloseTo(0.25, 6);
    expect(cells[1].x).toBeCloseTo(0.5, 6);
    expect(cells[2].x).toBeCloseTo(0.75, 6);
  });

  it('keeps every cell on the map, including the one behind the user', () => {
    // The seam is where azimuth wraps, and a cell landing at exactly 1 or 0 is on the map rather
    // than off it — clamping the wrong way would drop the cell directly behind the user.
    for (const azimuth of [179, -179, 180]) {
      const [behind] = mapCoverage(plan(cell(1 as NodeId, azimuth, 0)), nothingCovered);
      expect(behind.x).toBeGreaterThanOrEqual(0);
      expect(behind.x).toBeLessThanOrEqual(1);
    }
  });

  it('marks the cells the core called holes and no others', () => {
    // Which cells are holes is the planner's answer, not a threshold this client re-derives.
    const covered = mapCoverage(
      plan(cell(1 as NodeId, 0, 0), cell(2 as NodeId, 30, 0)),
      coverage(2));
    expect(covered.find((c) => c.node === 1)?.state).toBe('covered');
    expect(covered.find((c) => c.node === 2)?.state).toBe('hole');
  });
});
