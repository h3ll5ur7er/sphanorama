// A capture interrupted by a phone call is only recoverable if the page knows to offer it back.
// These pin the two judgements the client makes about that: which project to offer, and what to
// say when the core refuses to give it back.
import { describe, expect, it } from 'vitest';

import type { ProjectId, ProjectSummary } from '../../../../contracts/ts/contracts';
import { describeResumeRefusal, resumableProject } from './resume';

function project(id: number, hasSession: boolean, title = `sphere ${id}`): ProjectSummary {
  return {
    id: id as ProjectId,
    title,
    createdAtMs: 0,
    nodesTotal: 0,
    nodesSatisfied: 0,
    hasBuild: false,
    hasSession,
  };
}

describe('resumableProject', () => {
  it('offers nothing when no project has been captured into', () => {
    expect(resumableProject([])).toBeNull();
    expect(resumableProject([project(1, false), project(2, false)])).toBeNull();
  });

  it('offers the project that has a session written down', () => {
    expect(resumableProject([project(1, false), project(2, true)])).toBe(2);
  });

  it('offers the newest capture rather than whichever the store listed first', () => {
    // The spill tier holds one capture's frames: `Begin` empties it, so the newest session is the
    // only one whose pixels are still down there (ADR 0034). Offering an older one would restore
    // a coverage map whose frames the tier no longer has. The store's listing is insertion-
    // ordered rather than sorted — `DocumentHost.projectIds` walks a Map — so the order it comes
    // back in is not the order they were made in, and identity is what says which is newer.
    expect(resumableProject([project(2, true), project(7, true), project(5, true)])).toBe(7);
  });

  it('is not hidden by a project made after it that was never captured into', () => {
    // `project.create` alone writes no session document, so a project without one never emptied
    // the tier and cannot be what is in it. Taking the newest project outright — rather than the
    // newest *with a session* — would answer "nothing to resume" for a capture that is still
    // there in full.
    expect(resumableProject([project(4, true), project(9, false)])).toBe(4);
  });
});

describe('describeResumeRefusal', () => {
  const unreadable = {
    code: 'Unsupported' as const,
    component: 'CaptureSessionManager',
    detail: "this project's session document is from a shape this build cannot read",
  };
  const wrongTier = {
    code: 'FailedPrecondition' as const,
    component: 'CaptureSessionManager',
    detail: "this project's session was captured into a spill tier this device no longer holds",
  };

  it('says what the core said rather than a sentence about https', () => {
    // `Unsupported` is the core's word for "this build does not read that shape" and the camera
    // adapter's word for "this page is not on a secure origin". A refusal rewritten into the
    // second would send someone after a certificate for a document that simply cannot be parsed.
    const { message } = describeResumeRefusal(unreadable);
    expect(message).toContain('shape this build cannot read');
    expect(message).not.toMatch(/https/i);
  });

  it('names the resume, so the reason is not read as a failure of whatever comes next', () => {
    const { message } = describeResumeRefusal({
      code: 'FrameStoreExhausted', component: 'MemoryFrameStoreAccess', detail: 'no room',
    });
    expect(message).toMatch(/resume/i);
    // And still the sentence the shell already gives that code, rather than a second wording of
    // it that would drift from the first.
    expect(message).toMatch(/memory for frames/i);
  });

  it('keeps the offer up for a refusal another attempt could clear', () => {
    // A tier mismatch is a statement about *this* tier rather than about those frames: a session
    // that could not take the resident pair and fell back to a tier of its own (ADR 0030) says
    // exactly this while its pixels are still on disk. Withdrawing the offer would make a device
    // that is momentarily wrong look like a capture that is gone.
    const refusal = describeResumeRefusal(wrongTier);
    expect(refusal.offerAgain).toBe(true);
    expect(refusal.message).toMatch(/try again/i);
  });

  it('takes the offer down for a refusal only a new build can change', () => {
    // Nothing the user can do in this tab reads a document shape this build does not know, so an
    // offer left up is one that fails identically every time it is pressed. It is withdrawn for
    // this tab and no longer, which is the whole reason the page decides it rather than the core:
    // a remembered refusal that outlived the build would suppress the offer in the one version
    // that could finally honour it.
    const refusal = describeResumeRefusal(unreadable);
    expect(refusal.offerAgain).toBe(false);
    expect(refusal.message).not.toMatch(/try again/i);
    // And says the capture is still there. `Resume` keeps the document it cannot parse, so the
    // sentence that ends the offer must not read as "your sphere was thrown away".
    expect(refusal.message).toMatch(/kept/i);
  });

  it('reads the code rather than the component to decide whether pressing again helps', () => {
    // The camera adapter's `Unsupported` is the secure-origin one, which a second press does not
    // fix either — the origin is the page's own. Classifying by component would make this the
    // exception, and the exception would be wrong.
    expect(describeResumeRefusal({
      code: 'Unsupported', component: 'CameraAccess', detail: 'insecure origin',
    }).offerAgain).toBe(false);
  });
});
