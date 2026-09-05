/**
 * Whether there is a capture to come back to, and what to say when there was and it refused.
 *
 * The page must not find this out by trying. `ICaptureSessionManager::Resume` reads its document
 * before it touches anything, so a failed probe is cheap — but a *successful* one opens the
 * camera and starts tracking, which is a resume nobody asked for. So the question is answered
 * from `ProjectManager::List`, which reports it as a fact about the project and hands back no
 * session (ADR 0036).
 *
 * Both functions here are decisions rather than rendering, which is why they are testable
 * separately from the page that calls them.
 */
import type { ProjectId, ProjectSummary } from '../../../../contracts/ts/contracts';
import type { Status } from '../../access/result';
import { describeFailure } from './status';

/**
 * The capture worth offering back, or null when there is none.
 *
 * The newest one that has a session, and "newest" is the largest identity: `ProjectManager`
 * issues ids one past the highest the store holds, so they ascend with creation, while the
 * listing's *order* is whatever the document host's key map happened to iterate.
 *
 * Newest rather than any, because only one capture's frames are in the spill tier: `Begin`
 * empties it, so an older session's document would come back naming frames the tier no longer
 * has (ADR 0034). One unfinished sphere is also the case this is for — resuming means standing in
 * the same spot again, which is a phone call, not tomorrow.
 */
export function resumableProject(projects: readonly ProjectSummary[]): ProjectId | null {
  let newest: ProjectSummary | null = null;
  for (const project of projects) {
    if (!project.hasSession) continue;
    if (newest === null || (project.id as number) > (newest.id as number)) newest = project;
  }
  return newest === null ? null : newest.id;
}

/**
 * A refused resume, in words, naming what it was.
 *
 * `describeFailure` rather than a second wording of the same codes: it is the shell's one place
 * for turning a `Status` into a sentence, and a resume can refuse with codes it already covers.
 * What is added is the subject — the same status arriving from a resume and from a fresh capture
 * means different things to do next, and the line has to say which one just failed.
 */
export function describeResumeRefusal(status: Status): string {
  return `Could not resume that capture — ${describeFailure(status)}`;
}
