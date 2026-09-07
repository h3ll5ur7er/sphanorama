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
 * What a refused resume leaves behind: the sentence to show, and whether to offer it again.
 *
 * The two travel together because they are the same judgement. A message telling the user to try
 * again beside a button that is gone — or a button still up under a sentence saying it will never
 * work — is the pair disagreeing, and they can only disagree if something classifies the refusal
 * twice.
 */
export interface ResumeRefusal {
  /** For the stage line: what the core said, and what there is to do about it. */
  message: string;
  /** Whether to put the resume offer back up, so a second press is possible. */
  offerAgain: boolean;
  /**
   * Whether starting a *different* capture could work, which is not the same question.
   *
   * Most refusals are about this project — a document this build cannot read, frames the tier
   * lost — and a fresh sphere is unaffected, so the way out stays on offer. `SensorUnavailable`
   * is about the device: nothing on this page load can capture anything, so offering a new
   * capture invites a press that fails exactly as the resume just did. That was the case the
   * "two dead buttons" reasoning was written for and did not cover, because only the resume
   * offer was ever taken down.
   */
  offerFresh: boolean;
}

/**
 * A refused resume, in words, naming what it was and whether pressing again could change it.
 *
 * `describeFailure` rather than a second wording of the same codes: it is the shell's one place
 * for turning a `Status` into a sentence, and a resume can refuse with codes it already covers.
 * What is added is the subject — the same status arriving from a resume and from a fresh capture
 * means different things to do next, and the line has to say which one just failed.
 *
 * The offer comes back for every refusal except `Unsupported` and `SensorUnavailable`, and the
 * asymmetry is the answer to
 * the question ADR 0035 and ADR 0036 left between them (ADR 0039). Most refusals are statements
 * about this attempt: a tier this device does not currently hold, a store that would not take the
 * frames back, a camera another tab has. Those can be different on the next press or the next
 * load, and a capture that is still on disk must not be made to look gone. `Unsupported` is the
 * core's word for "this build does not read that", and no press changes which build is running.
 *
 * Read off the code rather than the component on purpose. The camera adapter's `Unsupported` is
 * the secure-origin one, which is equally not fixable by pressing again — the origin belongs to
 * the page — so the one component that means something different by the code still belongs on
 * the same side of this line.
 *
 * And withdrawn for *this tab* rather than remembered, which is the load-bearing half. The only
 * thing that turns an unreadable document into a readable one is a new version of the app, and a
 * refusal written down anywhere durable would survive into exactly that version and suppress the
 * offer it was finally able to honour. So the core is not asked to record it and the page does
 * not persist it: the offer is rebuilt from `hasSession` on every load, and a build that can read
 * the document simply resumes it.
 */
export function describeResumeRefusal(status: Status): ResumeRefusal {
  // `SensorUnavailable` joined `Unsupported` on the withdrawing side with ADR 0044, and for the
  // same reason rather than a similar one: nothing a press can do changes it. The host is told
  // the motion capability inside `enable`, once, and a second press of either button re-runs the
  // same refusal against the same answer — so "try again, or start a new one" offered two dead
  // buttons under a sentence that had just said to change a setting and reload. What makes the
  // capture readable again is the reload, which rebuilds the offer from `hasSession` anyway.
  const stuck = status.code === 'Unsupported' || status.code === 'SensorUnavailable';
  // Only this one says nothing else can start either. `Unsupported` is a fact about the stored
  // document, so a new sphere is untouched by it and the way out stays on offer.
  const deviceCannotCapture = status.code === 'SensorUnavailable';
  const next = status.code === 'Unsupported'
    ? 'this version cannot pick it up — the capture is kept for one that can'
    : deviceCannotCapture
      ? 'the capture is kept until this device can place its frames'
      : 'try again, or start a new one';
  return { message: `Could not resume that capture — ${describeFailure(status)} · ${next}`,
           offerAgain: !stuck,
           offerFresh: !deviceCannotCapture };
}
