// What the capture overlay decides to draw: a ring on every cell in view, filled if it has been
// captured, and an arrow when the cell to go to is not in view at all. How any of it looks is
// reviewed by eye; that it names the right cells, in the right places, with the right fill, is not.
import { describe, expect, it } from 'vitest';

import { planOverlay } from './overlay';
import type {
  CapturePlan, CoverageNode, CoverageState, NodeId, Quat,
} from '../../../../contracts/ts/contracts';

const LENS = { horizontalFovDeg: 66, verticalFovDeg: 52 };

function attitude(azimuthDeg: number, elevationDeg: number): Quat {
  const yaw = (azimuthDeg * Math.PI) / 360;
  const pitch = (elevationDeg * Math.PI) / 360;
  const [cy, sy] = [Math.cos(yaw), Math.sin(yaw)];
  const [cp, sp] = [Math.cos(pitch), Math.sin(pitch)];
  return { w: cy * cp, x: cy * sp, y: sy * cp, z: -sy * sp };
}

function cell(id: number, azimuthDeg: number, elevationDeg: number): CoverageNode {
  return {
    id, targetOrientation: attitude(azimuthDeg, elevationDeg), acceptanceConeDeg: 5, ringIndex: 0,
  } as unknown as CoverageNode;
}

function plan(...nodes: CoverageNode[]): CapturePlan {
  return { nodes, spec: LENS } as unknown as CapturePlan;
}

function coverage(...holes: number[]): CoverageState {
  return {
    nodesTotal: 0, nodesSatisfied: 0, coveredSolidAngleFraction: 0,
    holes: holes as NodeId[], underOverlapped: [],
  };
}

const ahead = attitude(0, 0);

describe('planOverlay', () => {
  it('rings only the cells that are actually in the picture', () => {
    // A marker for a cell behind the user is not a marker, it is a lie about where things are.
    const overlay = planOverlay({
      plan: plan(cell(1, 0, 0), cell(2, 180, 0)),
      coverage: coverage(1, 2),
      attitude: ahead,
      targetNode: 1 as NodeId,
    });
    expect(overlay.rings.map((ring) => ring.node)).toEqual([1]);
  });

  it('fills the ring of a cell that has been captured and leaves a hole empty', () => {
    const overlay = planOverlay({
      plan: plan(cell(1, -10, 0), cell(2, 10, 0)),
      coverage: coverage(2),          // cell 1 captured, cell 2 still a hole
      attitude: ahead,
      targetNode: 2 as NodeId,
    });
    expect(overlay.rings.find((ring) => ring.node === 1)?.fill).toBe(1);
    expect(overlay.rings.find((ring) => ring.node === 2)?.fill).toBe(0);
  });

  it('fills the cell being aimed at part way, for a hold that is under way', () => {
    // The affordance the ring exists to have: nothing drives a middle value yet, but the shape
    // that would — a hold-still timer counting toward a burst — reads as a filling ring rather
    // than as a number, and a fill that only ever took two values would have to be rebuilt.
    const overlay = planOverlay({
      plan: plan(cell(1, 0, 0)),
      coverage: coverage(1),
      attitude: ahead,
      targetNode: 1 as NodeId,
      holding: 0.4,
    });
    expect(overlay.rings[0].fill).toBeCloseTo(0.4, 9);
  });

  it('does not let a hold overwrite a cell that is already captured', () => {
    // Progress toward capturing something is meaningless once it is captured, and a ring that
    // emptied itself when the user lingered would read as losing the frame they just took.
    const overlay = planOverlay({
      plan: plan(cell(1, 0, 0)),
      coverage: coverage(),           // nothing is a hole: cell 1 is captured
      attitude: ahead,
      targetNode: 1 as NodeId,
      holding: 0.4,
    });
    expect(overlay.rings[0].fill).toBe(1);
  });

  it('marks which ring is the one being aimed at', () => {
    const overlay = planOverlay({
      plan: plan(cell(1, -10, 0), cell(2, 10, 0)),
      coverage: coverage(1, 2),
      attitude: ahead,
      targetNode: 2 as NodeId,
    });
    expect(overlay.rings.find((ring) => ring.node === 2)?.isTarget).toBe(true);
    expect(overlay.rings.find((ring) => ring.node === 1)?.isTarget).toBe(false);
  });

  it('puts the rings where the sighting puts them', () => {
    const overlay = planOverlay({
      plan: plan(cell(1, 0, 0)),
      coverage: coverage(1),
      attitude: ahead,
      targetNode: 1 as NodeId,
    });
    expect(overlay.rings[0].x).toBeCloseTo(0.5, 6);
    expect(overlay.rings[0].y).toBeCloseTo(0.5, 6);
  });

  it('raises an arrow only when the cell to go to is out of sight', () => {
    const inView = planOverlay({
      plan: plan(cell(1, 0, 0)),
      coverage: coverage(1),
      attitude: ahead,
      targetNode: 1 as NodeId,
    });
    expect(inView.arrow).toBeNull();

    const behind = planOverlay({
      plan: plan(cell(1, 180, 0)),
      coverage: coverage(1),
      attitude: ahead,
      targetNode: 1 as NodeId,
    });
    expect(behind.arrow).not.toBeNull();
  });

  it('points the arrow the way the user has to turn', () => {
    const overlay = planOverlay({
      plan: plan(cell(1, -100, 0)),
      coverage: coverage(1),
      attitude: ahead,
      targetNode: 1 as NodeId,
    });
    expect(overlay.arrow?.bearingDeg).toBeCloseTo(90, 4);
  });

  it('draws nothing at all rather than guessing when the plan names no lens', () => {
    // Begin resolves the field of view from the camera, so a zero here means the plan was never
    // sized — and markers placed through a zero-width frustum land on infinity.
    const overlay = planOverlay({
      plan: { nodes: [cell(1, 0, 0)], spec: { horizontalFovDeg: 0, verticalFovDeg: 0 } } as unknown as CapturePlan,
      coverage: coverage(1),
      attitude: ahead,
      targetNode: 1 as NodeId,
    });
    expect(overlay.rings).toEqual([]);
    expect(overlay.arrow).toBeNull();
  });

  it('says nothing about a target the plan does not contain', () => {
    const overlay = planOverlay({
      plan: plan(cell(1, 0, 0)),
      coverage: coverage(1),
      attitude: ahead,
      targetNode: 99 as NodeId,
    });
    expect(overlay.arrow).toBeNull();
    expect(overlay.rings.map((ring) => ring.node)).toEqual([1]);
  });
});
