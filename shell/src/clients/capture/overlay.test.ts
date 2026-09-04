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

describe('planOverlay against the box the video is painted in', () => {
  // The overlay's markers are fractions of the camera frame; the page paints that frame into a
  // box of a different shape with `object-fit: cover`. Reported from a phone, where the AR "took
  // a bit to get used to": every marker was a little in from where the thing it named actually
  // was, and the cell being aimed at did not sit under the reticle.
  const PORTRAIT = { frameWidth: 960, frameHeight: 1280, boxWidth: 900, boxHeight: 1650 };

  it('keeps the cell being aimed at under the reticle, whatever the box', () => {
    // The one marker whose position can be checked by eye on a phone: point at a cell and its
    // ring has to land on the centre of the picture, because that is what the reticle marks.
    const overlay = planOverlay({
      plan: plan(cell(1, 0, 0)),
      coverage: coverage(1),
      attitude: ahead,
      targetNode: 1 as NodeId,
      fit: PORTRAIT,
    });
    expect(overlay.rings[0].x).toBeCloseTo(0.5, 6);
    expect(overlay.rings[0].y).toBeCloseTo(0.5, 6);
  });

  it('pushes a marker out to where the crop actually put the thing it names', () => {
    const off = { plan: plan(cell(1, -25, 0)), coverage: coverage(1), attitude: ahead,
                  targetNode: 1 as NodeId };
    const unfitted = planOverlay(off);
    const fitted = planOverlay({ ...off, fit: PORTRAIT });

    expect(unfitted.rings[0].x).toBeGreaterThan(0.5);
    expect(fitted.rings[0].x).toBeGreaterThan(unfitted.rings[0].x);
  });
});

describe('the arrow to a cell out of sight', () => {
  it('says how far the phone has to turn, not just which way', () => {
    // Two cells that point the arrow the same way and are nothing like the same distance. Without
    // a number the user turns and turns with nothing on screen saying how much is left, which is
    // what "the arrow was not as intuitive as I hoped" turned out to mean.
    const near = planOverlay({
      plan: plan(cell(1, -95, 0)), coverage: coverage(1), attitude: ahead, targetNode: 1 as NodeId,
    });
    const far = planOverlay({
      plan: plan(cell(1, -170, 0)), coverage: coverage(1), attitude: ahead, targetNode: 1 as NodeId,
    });

    expect(near.arrow?.awayDeg).toBeCloseTo(95, 3);
    expect(far.arrow?.awayDeg).toBeCloseTo(170, 3);
  });

  it('sits out toward the edge on the side the cell is, rather than in the middle', () => {
    // An arrow in the centre of the picture is a compass; one out at the edge is a signpost. The
    // second is the one that reads as "over there" without being decoded.
    const right = planOverlay({
      plan: plan(cell(1, -120, 0)), coverage: coverage(1), attitude: ahead, targetNode: 1 as NodeId,
    });
    const left = planOverlay({
      plan: plan(cell(1, 120, 0)), coverage: coverage(1), attitude: ahead, targetNode: 1 as NodeId,
    });

    expect(right.arrow!.x).toBeGreaterThan(0.8);
    expect(right.arrow!.x).toBeLessThanOrEqual(1);
    expect(right.arrow!.y).toBeCloseTo(0.5, 2);
    expect(left.arrow!.x).toBeLessThan(0.2);
  });

  it('stays on screen when the frame is cropped into the box', () => {
    // The arrow is not a point in the camera frame the way a ring is — it is a signpost placed at
    // a distance from the centre of the *screen*. Sending it through the crop as well marched it
    // past the edge of the phone on whichever axis the fit had trimmed.
    const overlay = planOverlay({
      plan: plan(cell(1, -120, 0)), coverage: coverage(1), attitude: ahead, targetNode: 1 as NodeId,
      fit: { frameWidth: 960, frameHeight: 1280, boxWidth: 900, boxHeight: 1650 },
    });

    expect(overlay.arrow!.x).toBeGreaterThan(0.8);
    expect(overlay.arrow!.x).toBeLessThanOrEqual(1);
  });

  it('does not point at a cell that has already been captured', () => {
    // Seen on a finished sphere: 28 of 28 done, and an arrow still sending the user off to a cell
    // there was nothing left to do at. Guidance keeps naming a nearest node once everything is
    // covered, which is the right answer to a different question.
    const overlay = planOverlay({
      plan: plan(cell(1, 180, 0)),
      coverage: coverage(),
      attitude: ahead,
      targetNode: 1 as NodeId,
    });
    expect(overlay.arrow).toBeNull();
  });
});
