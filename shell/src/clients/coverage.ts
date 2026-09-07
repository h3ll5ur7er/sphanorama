/**
 * What "this cell is done" means, in one place.
 *
 * Two renderings of one sphere ask it — the reticle's rings and the review panel's map — and they
 * were asking it separately: `!holes.has(id)` written out in `capture/overlay.ts` and
 * `holes.has(id)` in `review/coverage-map.ts`. They shared the `CoverageState` value and not the
 * rule that reads it, which is the half that decides. The moment "covered" stops being "absent from
 * `holes`" — `underOverlapped` counting as not-really-done, a state that lists members instead of
 * holes, a per-cell quality bar — one of the two moves and the other does not, and the symptom is a
 * green ring over a hollow dot with nothing to say which is right.
 *
 * It is deliberately the *client's* reading of an answer the core computed, not a second opinion
 * about coverage: `CoverageState` comes from `ICoveragePlannerEngine::Evaluate`, which is where
 * what counts as covered is decided (ADR 0027). This only says how to read it.
 */
import type { CoverageState, NodeId } from '../../../contracts/ts/contracts';

/** The cells still wanting a capture, as a set, built once per render rather than per cell. */
export function holesOf(coverage: CoverageState): Set<number> {
  return new Set<number>(coverage.holes as readonly number[]);
}

/** Whether this cell already holds a capture. */
export function isCovered(holes: Set<number>, node: NodeId): boolean {
  return !holes.has(node as number);
}
