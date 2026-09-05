/**
 * A cell's burst as a strip to choose from.
 *
 * Two facts about each candidate and neither is this client's to invent. The *order* is
 * `IFrameQualityEngine::Rank`'s answer, handed over by `CaptureSessionManager.Candidates`
 * best-first — sorting here would be a client deciding what "best" means, which is V6's and the
 * most-tuned part of the system. What is *in force* is the manual selection when there is one and
 * the automatic pick otherwise, which is the rule `ProjectManager.SetSelection` implements on the
 * other side (UC-3).
 */
import type { Candidate, CandidateId } from '../../../../contracts/ts/contracts';

/**
 * What the core answered when asked which candidate a cell has recorded.
 *
 * An identity, `null` for a caller with no number to pass, or `unreadable` when the read itself
 * failed and there is therefore no answer to honour.
 *
 * Zero needs no case of its own. It is an identity no candidate can have and it is the core's
 * word for "nobody has chosen here" (ADR 0040), so it falls through the `ranked.some` below into
 * the ranking, alongside a selection whose candidate a retake forgot — which is where it belongs,
 * because the build does the same thing with both.
 */
export type Recorded = CandidateId | null | 'unreadable';

export interface StripEntry {
  candidate: CandidateId;
  quality: Candidate['quality'];
  /** First in the ranking: what the build uses unless somebody says otherwise. */
  isAutomaticPick: boolean;
  /** What the build will actually use — the manual choice, or the automatic pick. */
  isInForce: boolean;
}

/**
 * Which candidate the strip marks as in force, or nothing when that cannot be known.
 *
 * A selection can outlive the candidate it names: a replace-retake forgets a cell's frames and
 * the recorded choice is not cleaned up with them. Falling back to the ranking is what the build
 * would do anyway, and showing nothing in force would say the cell has no frame.
 *
 * A read that *failed* is the one case where showing nothing is right. It is not an answer of
 * "nobody has chosen", and marking a row on the strength of it would be this client inventing the
 * single fact the strip exists to report. `undefined` is how that is said, and it marks nothing
 * without needing a case of its own below: no candidate identity is equal to it. The panel puts
 * the reason in the heading, so an unmarked strip is not left to be read as an empty one.
 */
function honouredId(
  ranked: readonly Candidate[], selected: Recorded): CandidateId | undefined {

  if (selected === 'unreadable') return undefined;
  if (selected !== null && ranked.some((c) => c.id === selected)) return selected;
  return ranked[0]?.id;
}

export function candidateStrip(
  ranked: readonly Candidate[], selected: Recorded,
): StripEntry[] {
  const honoured = honouredId(ranked, selected);

  return ranked.map((candidate, position) => ({
    candidate: candidate.id,
    quality: candidate.quality,
    isAutomaticPick: position === 0,
    isInForce: candidate.id === honoured,
  }));
}
