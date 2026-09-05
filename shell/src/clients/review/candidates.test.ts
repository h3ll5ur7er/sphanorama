// The per-cell strip: what a burst offers, in the order the core ranked it, with the automatic
// pick and any manual override named. Every assertion here is about a decision — nothing about
// what a thumbnail looks like, which is reviewed by eye.
import { describe, expect, it } from 'vitest';

import { candidateStrip } from './candidates';
import type { Candidate, CandidateId } from '../../../../contracts/ts/contracts';

function candidate(id: number, sharpness: number): Candidate {
  return {
    id,
    node: 1,
    frame: { id },
    quality: { sharpness, exposureAgreement: 1, motionBlur: 0, aggregate: sharpness },
  } as unknown as Candidate;
}

// Best-first, as CaptureSessionManager.Candidates hands them over.
const ranked = [candidate(7, 0.9), candidate(3, 0.5), candidate(9, 0.2)];

describe('candidateStrip', () => {
  it('names the first as the automatic pick, because the core ranked it there', () => {
    // The client does not decide what "best" means — that is the quality engine's, and the strip
    // would be re-deriving it from scores if it sorted here.
    const strip = candidateStrip(ranked, null);
    expect(strip.map((entry) => entry.candidate)).toEqual([7, 3, 9]);
    expect(strip[0].isAutomaticPick).toBe(true);
    expect(strip.filter((entry) => entry.isAutomaticPick)).toHaveLength(1);
  });

  it('shows the automatic pick as the one in force when nobody has chosen', () => {
    const strip = candidateStrip(ranked, null);
    expect(strip[0].isInForce).toBe(true);
    expect(strip.filter((entry) => entry.isInForce)).toHaveLength(1);
  });

  it('moves what is in force to a manual choice without reordering the strip', () => {
    // The order is the ranking's answer and stays put: a strip that reshuffled itself when you
    // picked something would lose the comparison you were making.
    const strip = candidateStrip(ranked, 9 as CandidateId);
    expect(strip.map((entry) => entry.candidate)).toEqual([7, 3, 9]);
    expect(strip[2].isInForce).toBe(true);
    expect(strip[0].isInForce).toBe(false);
    // And the automatic pick is still named, because that is what the override is against.
    expect(strip[0].isAutomaticPick).toBe(true);
  });

  it('falls back to the automatic pick when the selection names a frame that is gone', () => {
    // A replace-retake forgets a cell's candidates, and a selection recorded against one of them
    // outlives it. Showing nothing in force would say the cell has no frame, which is worse than
    // showing the one the core would use.
    const strip = candidateStrip(ranked, 404 as CandidateId);
    expect(strip[0].isInForce).toBe(true);
  });

  it('treats the zero the core answers for an unchosen cell as no choice at all', () => {
    // `GetSelection` answers `Ok(0)` for "nobody has chosen here" (ADR 0040) and the panel passes
    // it straight through rather than translating it, so this file is where it has to land in the
    // right place. Zero is an identity no candidate can have, so it takes the same fallback as a
    // selection whose candidate a retake forgot — which is only true as long as nothing here
    // treats a number as a choice merely because it is a number.
    const strip = candidateStrip(ranked, 0 as CandidateId);
    expect(strip[0].isInForce).toBe(true);
    expect(strip.filter((entry) => entry.isInForce)).toHaveLength(1);
  });

  it('marks nothing in force when the recorded selection could not be read', () => {
    // Not the same as nobody having chosen. Falling back to the ranking would say "this is what
    // the build will use" about a cell whose override nobody could read — the screen disagreeing
    // with the build, silently, which is the failure ADR 0040 exists to end. The panel says so in
    // the heading; what this file owes is not to mark a row.
    const strip = candidateStrip(ranked, 'unreadable');
    expect(strip.map((entry) => entry.candidate)).toEqual([7, 3, 9]);
    expect(strip.filter((entry) => entry.isInForce)).toHaveLength(0);
    // The automatic pick is still named. It is a fact about the ranking, which was read fine.
    expect(strip[0].isAutomaticPick).toBe(true);
  });

  it('is empty rather than broken for a cell nobody has captured', () => {
    expect(candidateStrip([], null)).toEqual([]);
    expect(candidateStrip([], 7 as CandidateId)).toEqual([]);
    expect(candidateStrip([], 'unreadable')).toEqual([]);
  });
});
