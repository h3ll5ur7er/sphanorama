// The generated codec, TypeScript half.
//
// The round trips prove this emitter is self-consistent. The golden payload proves something
// stronger and more useful: it is the exact hex the C++ suite produces in
// core/test/codec_test.cpp, so the two generated halves are pinned to each other. If they ever
// disagree about field order or widths, one of these two tests fails instead of a message
// decoding into plausible nonsense in a browser.
import { describe, expect, it } from 'vitest';

import { decodeCaptureGuidance, decodeCapturePlan, encodeCaptureGuidance, encodeCapturePlan }
  from './codec.generated';
import { Reader, Writer } from './wire';
import type * as C from '../../../contracts/ts/contracts';

const GOLDEN_CAPTURE_GUIDANCE =
  '0000000000001c4000000000000029400000000000000ac0000000000000e03f01000000';

function fromHex(hex: string): Uint8Array {
  return new Uint8Array((hex.match(/../g) ?? []).map((pair) => parseInt(pair, 16)));
}

function toHex(bytes: Uint8Array): string {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

describe('cross-language wire format', () => {
  it('decodes the payload the C++ side produces', () => {
    const guidance = decodeCaptureGuidance(new Reader(fromHex(GOLDEN_CAPTURE_GUIDANCE)));
    expect(guidance.targetNode).toBe(7);
    expect(guidance.angularErrorDeg).toBe(12.5);
    expect(guidance.rollErrorDeg).toBe(-3.25);
    expect(guidance.stability).toBe(0.5);
    expect(guidance.action).toBe('HoldStill');
  });

  it('produces the payload the C++ side expects', () => {
    const writer = new Writer();
    encodeCaptureGuidance(writer, {
      targetNode: 7 as C.NodeId,
      angularErrorDeg: 12.5,
      rollErrorDeg: -3.25,
      stability: 0.5,
      action: 'HoldStill',
    });
    expect(toHex(writer.finish())).toBe(GOLDEN_CAPTURE_GUIDANCE);
  });
});

describe('round trips', () => {
  it('round-trips a flat struct', () => {
    const writer = new Writer();
    const original: C.CaptureGuidance = {
      targetNode: 3 as C.NodeId,
      angularErrorDeg: 1.5,
      rollErrorDeg: 0,
      stability: 0.25,
      action: 'Seek',
    };
    encodeCaptureGuidance(writer, original);
    expect(decodeCaptureGuidance(new Reader(writer.finish()))).toEqual(original);
  });

  it('round-trips nested structs and arrays', () => {
    const writer = new Writer();
    const plan: C.CapturePlan = {
      nodes: [
        { id: 1 as C.NodeId, targetOrientation: { w: 1, x: 0, y: 0, z: 0 }, acceptanceConeDeg: 4, ringIndex: 0 },
        { id: 2 as C.NodeId, targetOrientation: { w: 0, x: 1, y: 0, z: 0 }, acceptanceConeDeg: 5, ringIndex: 1 },
      ],
      spec: {
        strategy: 'Geodesic',
        horizontalFovDeg: 66,
        verticalFovDeg: 50,
        overlapTarget: 0.3,
        acceptanceConeDeg: 4,
        coverPoles: true,
        motion: 'GyroAccel',
      },
    };
    encodePlan(writer, plan);
    expect(decodeCapturePlan(new Reader(writer.finish()))).toEqual(plan);
  });

  it('round-trips an empty array', () => {
    const writer = new Writer();
    const plan: C.CapturePlan = {
      nodes: [],
      spec: {
        strategy: 'Rings', horizontalFovDeg: 0, verticalFovDeg: 0, overlapTarget: 0,
        acceptanceConeDeg: 4, coverPoles: false, motion: 'None',
      },
    };
    encodePlan(writer, plan);
    expect(decodeCapturePlan(new Reader(writer.finish())).nodes).toEqual([]);
  });

  it('rejects a truncated payload rather than half-decoding it', () => {
    const writer = new Writer();
    encodeCaptureGuidance(writer, {
      targetNode: 1 as C.NodeId, angularErrorDeg: 1, rollErrorDeg: 1, stability: 1, action: 'Seek',
    });
    const full = writer.finish();
    const reader = new Reader(full.subarray(0, full.length - 4));
    decodeCaptureGuidance(reader);
    expect(reader.ok).toBe(false);
  });
});

function encodePlan(writer: Writer, plan: C.CapturePlan) {
  encodeCapturePlan(writer, plan);
}
