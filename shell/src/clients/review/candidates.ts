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

export interface StripEntry {
  candidate: CandidateId;
  quality: Candidate['quality'];
  /** First in the ranking: what the build uses unless somebody says otherwise. */
  isAutomaticPick: boolean;
  /** What the build will actually use — the manual choice, or the automatic pick. */
  isInForce: boolean;
}

export function candidateStrip(
  ranked: readonly Candidate[], selected: CandidateId | null,
): StripEntry[] {
  // A selection can outlive the candidate it names: a replace-retake forgets a cell's frames and
  // the recorded choice is not cleaned up with them. Falling back to the ranking is what the
  // build would do anyway, and showing nothing in force would say the cell has no frame.
  const honoured = selected !== null && ranked.some((c) => c.id === selected)
    ? selected
    : ranked[0]?.id;

  return ranked.map((candidate, position) => ({
    candidate: candidate.id,
    quality: candidate.quality,
    isAutomaticPick: position === 0,
    isInForce: candidate.id === honoured,
  }));
}
