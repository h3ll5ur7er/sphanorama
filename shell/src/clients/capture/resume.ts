/**
 * Whether there is a capture to come back to, and what to say when there was and it refused.
 *
 * The page must not find this out by trying. `ICaptureSessionManager::Resume` reads its document
 * before it touches anything, so a failed probe is cheap — but a *successful* one opens the
 * camera and starts tracking, which is a resume nobody asked for. So the question is answered
 * from `ProjectManager::List`, which reports it as a fact about the project and hands back no
 * session (ADR 0036).
 *
 * The functions here are decisions rather than rendering, which is why they are testable
 * separately from the page that calls them.
 */
import type { ProjectId, ProjectSummary, Result } from '../../../../contracts/ts/contracts';
import type { Status } from '../../access/result';
import { describeFailure } from './status';

/**
 * The capture worth offering back, or null when there is none.
 *
 * The newest one that has a session, and "newest" is the largest identity: the store issues ids
 * from the clock and above every id it holds, so they ascend with creation, though across tabs
 * only by the second (ADR 0081), while the listing's *order* is whatever the document host's key
 * map happened to iterate.
 *
 * Newest rather than any, because only one capture's frames are in the spill tier: `Begin`
 * empties it, so an older session's document would come back naming frames the tier no longer
 * has (ADR 0034). One unfinished sphere is also the case this is for — resuming means standing in
 * the same spot again, which is a phone call, not tomorrow.
 *
 * And the newest in *this page's* tier. A tab beside the one holding the resident tier captures
 * into one of its own (ADR 0063), so its capture can be newer and still be one this page cannot
 * give back; offered, it was refused, and offered again on every load, in front of the capture
 * this tier does hold (ADR 0083).
 *
 * A capture this build cannot read comes last, offered only where no readable one in this tier is:
 * its refusal withdraws the offer and leaves a new capture, which would empty the tier a readable
 * one is in.
 */
export function resumableProject(projects: readonly ProjectSummary[]): ProjectId | null {
  let newest: ProjectSummary | null = null;
  let newestUnreadable: ProjectSummary | null = null;
  for (const project of projects) {
    if (!project.hasSession || project.sessionInAnotherTier) continue;
    if (project.sessionUnreadable) {
      if (newestUnreadable === null || (project.id as number) > (newestUnreadable.id as number)) {
        newestUnreadable = project;
      }
    } else if (newest === null || (project.id as number) > (newest.id as number)) {
      newest = project;
    }
  }
  return (newest ?? newestUnreadable)?.id ?? null;
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
 * asymmetry is the answer to the question ADR 0035 and ADR 0036 left between them (ADR 0039). Most
 * refusals are statements about this attempt: a store that would not take the frames back, a camera
 * another tab has. (A tier this device does not hold was one; since ADR 0083 such a capture is not
 * offered at all.) Those can be different on the next press or the next load, and a capture that is
 * still on disk must not be made to look gone. `Unsupported` is the core's word for "this build
 * does not read that", and no press changes which build is running.
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

/**
 * Whether the page holds a tier of its own, which is what `startOffer`'s hint is for: one that is
 * not the resident pair, and is a tier at all. A page with no tier is told apart, since it cannot
 * tell a device with no private file system from a failed fallback (ADR 0083).
 */
export function onFallbackTier(
  tier: { canSpill(): boolean; holdsResidentTier(): boolean },
): boolean {
  return tier.canSpill() && !tier.holdsResidentTier();
}

/**
 * What the page offers once the core is up: the capture to resume, if any, and the line saying
 * which presses are on offer.
 *
 * A listing that failed is not one with nothing in it. A new capture empties the spill tier
 * (ADR 0034), so a line inviting one after a listing that could not say whether a capture is
 * waiting invites the press that would erase it. The new capture stays on offer, since a store
 * that keeps failing must not lock the user out of capturing anything, and the line says what it
 * would cost (ADR 0078).
 *
 * `fallbackTier` is a page whose worker took a tier of its own rather than the resident one: a
 * second tab, or one opened while the last tab's worker was still letting go (ADR 0063). There a
 * capture in another tier may be the resident tier's, still on disk and offered by a reload that
 * gets the resident tier, so the line says one may be waiting. "May", because the page cannot tell
 * it from one the resident tier will not give back: an older capture it has since cleared, another
 * fallback tier's, one that named frames on a page with no tier, or one with no frames written on
 * a page with no tier, which only such a page offers. Where nothing readable is offered, that is,
 * since an unreadable capture can be offered on any tier. On the resident tier every capture in
 * another tier is one of those, and nothing is said (ADR 0083).
 */
export function startOffer(listed: Result<ProjectSummary[]>, storageError: string | null,
                           fallbackTier: boolean): {
  resume: ProjectId | null;
  message: string;
} {
  // A store that would not load lists nothing and fails nothing: the worker's host starts empty and
  // keeps the app running, so an empty listing is all the core can report. Said as a failed
  // listing is, since a capture may be waiting on disk all the same.
  if (storageError !== null) {
    return {
      resume: null,
      message:
        'core ready, but it could not tell whether a capture is waiting: the saved projects did ' +
        `not load (${storageError}) — reload to try again; a new capture would clear one ` +
        'that is, and would not be saved',
    };
  }
  if (!listed.ok) {
    return {
      resume: null,
      message:
        'core ready, but it could not tell whether a capture is waiting: ' +
        `${describeFailure(listed.status)} — reload to try again; a new capture would clear one ` +
        'that is',
    };
  }
  const resume = resumableProject(listed.value);
  const readableOffered = listed.value.some(
    (project) => project.id === resume && !project.sessionUnreadable);
  if (!readableOffered && fallbackTier
      && listed.value.some((project) => project.hasSession && project.sessionInAnotherTier)) {
    // An unreadable capture is still offered, and the line names that press first, apart from the
    // hint, so the hint does not read as what the resume button reaches.
    return {
      resume,
      message: resume === null
        ? 'core ready — a capture may be waiting in storage this page could not take: reload, ' +
          'closing any other tab of this app, to find out; or enable the camera to start a new one'
        : 'core ready — resume the last capture, or enable the camera to start a new one; a ' +
          'capture this page could not take may also be waiting: reload, closing any other tab ' +
          'of this app, to find out',
    };
  }
  return {
    resume,
    message: resume === null
      ? 'core ready — enable the camera to continue'
      : 'core ready — resume the last capture, or enable the camera to start a new one',
  };
}
