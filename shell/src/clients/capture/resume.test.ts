// A capture interrupted by a phone call is only recoverable if the page knows to offer it back.
// These pin the two judgements the client makes about that: which project to offer, and what to
// say when the core refuses to give it back.
import { describe, expect, it } from 'vitest';

import type { ProjectId, ProjectSummary } from '../../../../contracts/ts/contracts';
import { describeResumeRefusal, resumableProject, startOffer } from './resume';
import { describeFailure } from './status';

function project(id: number, hasSession: boolean, title = `sphere ${id}`,
                 sessionInAnotherTier = false, sessionUnreadable = false): ProjectSummary {
  return {
    id: id as ProjectId,
    title,
    createdAtMs: 0,
    nodesTotal: 0,
    nodesSatisfied: 0,
    hasBuild: false,
    hasSession,
    sessionInAnotherTier,
    sessionUnreadable,
  };
}

function elsewhere(id: number): ProjectSummary {
  return project(id, true, `sphere ${id}`, true);
}

function unreadable(id: number): ProjectSummary {
  return project(id, true, `sphere ${id}`, false, true);
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

  it('offers the newest capture this tier holds, not a newer one in another tab\'s', () => {
    // A tab beside the one holding the resident tier captures into a tier of its own (ADR 0063),
    // and its capture can have the larger id. `Resume` refuses that one, and a refusal for the tier
    // keeps the offer up (ADR 0039), so offering it hid the capture this tier holds on every load
    // (ADR 0083).
    expect(resumableProject([project(4, true), elsewhere(9)])).toBe(4);
    expect(resumableProject([elsewhere(9), project(4, true), project(2, true)])).toBe(4);
  });

  it('offers nothing when every capture is in another tier', () => {
    // A new capture here empties this tier, which holds none of them, so nothing is lost by it.
    expect(resumableProject([elsewhere(3), elsewhere(9)])).toBeNull();
  });

  it('offers a capture this build cannot read only where no readable one is', () => {
    // Offered, it is refused as `Unsupported` and the offer is withdrawn, leaving a new capture,
    // which empties the tier a readable capture is in (ADR 0083). Offered alone, its refusal says
    // it is kept for a build that can read it (ADR 0039).
    expect(resumableProject([project(4, true), unreadable(9)])).toBe(4);
    expect(resumableProject([unreadable(9), elsewhere(12)])).toBe(9);
    expect(resumableProject([unreadable(3), unreadable(9)])).toBe(9);
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

  it('takes the offer down when this device cannot place the frames at all', () => {
    // ADR 0044. A resume needs a motion sensor and the host is told the capability once, inside
    // `enable`, so a second press of either button re-runs the same refusal against the same
    // answer — two dead controls under a sentence that had just said to change a setting and
    // reload. What clears it is the reload, which rebuilds the offer from `hasSession` anyway.
    const refusal = describeResumeRefusal({
      code: 'SensorUnavailable', component: 'CaptureSessionManager',
      detail: 'this device reports no motion sensors',
    });
    expect(refusal.offerAgain).toBe(false);
    expect(refusal.message).not.toMatch(/try again/i);
    // And the capture is still there. `Resume` refuses before it touches the document, so the
    // sentence that ends the offer must not read as "your sphere was thrown away".
    expect(refusal.message).toMatch(/kept/i);
    // And neither is a fresh capture on offer, which is the half the first version of this missed:
    // the refusal is about the device rather than the document, so `Begin` fails exactly as
    // `Resume` just did and a way out that leads nowhere is one dead button instead of two.
    expect(refusal.offerFresh).toBe(false);
  });

  it('still offers a fresh capture for a refusal that is only about this document', () => {
    // The distinction `offerFresh` exists for. Nothing a press can do makes this build read that
    // document — so the resume offer goes — but a new sphere on the same device is untouched, and
    // taking that away too would strand a user who has a working camera and a working sensor.
    const refusal = describeResumeRefusal(unreadable);
    expect(refusal.offerAgain).toBe(false);
    expect(refusal.offerFresh).toBe(true);
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

describe('startOffer', () => {
  it('offers the newest capture with a session, and says both ways on', () => {
    const listed = { ok: true as const, value: [project(1, true), project(3, true)] };
    const offer = startOffer(listed, null, false);
    expect(offer.resume).toBe(3);
    expect(offer.message).toBe(
      'core ready — resume the last capture, or enable the camera to start a new one',
    );
  });

  it('says a capture is waiting where this page holds a tier of its own', () => {
    // A page that did not get the resident tier — a second tab, or a tab opened while the last
    // one's worker was still letting go of it (ADR 0063) — sees the resident tier's capture as in
    // another tier, and is not offered it. It is still on disk, and a reload with the other tab
    // closed offers it, so the line says so rather than that there is nothing (ADR 0083).
    const waiting = [project(1, false), elsewhere(3)];
    const fallback = startOffer({ ok: true, value: waiting }, null, true);
    expect(fallback.resume).toBeNull();
    expect(fallback.message).toContain('a capture is waiting');
    expect(fallback.message).toContain('reload');
    expect(fallback.message).not.toContain('enable the camera to continue');

    // On the resident tier a capture in another tier is a fallback tier's, which nobody will
    // ever hold again, so there is nothing to say about it.
    const resident = startOffer({ ok: true, value: waiting }, null, false);
    expect(resident.message).toBe('core ready — enable the camera to continue');
    // Nor where the fallback page has nothing in another tier.
    const nothing = startOffer({ ok: true, value: [project(1, false)] }, null, true);
    expect(nothing.message).toBe('core ready — enable the camera to continue');
    // And an offer is an offer, whichever tier the page holds.
    const offered = startOffer({ ok: true, value: [project(2, true), elsewhere(3)] }, null, true);
    expect(offered.resume).toBe(2);
    expect(offered.message).toBe(
      'core ready — resume the last capture, or enable the camera to start a new one',
    );
  });

  it('offers a new capture alone when nothing was captured into', () => {
    const offer = startOffer({ ok: true, value: [project(1, false)] }, null, false);
    expect(offer.resume).toBeNull();
    expect(offer.message).toBe('core ready — enable the camera to continue');
  });

  it('does not read a listing that failed as one with nothing to resume', () => {
    // A new capture empties the spill tier, so if a capture is waiting in it the press that the
    // old line invited would erase it. The listing could not say, so the line says that instead
    // of "continue" (ADR 0078).
    const status = {
      code: 'StorageQuotaExceeded', component: 'ProjectManager', detail: 'full',
    } as const;
    const offer = startOffer({ ok: false, status }, null, false);
    expect(offer.resume).toBeNull();
    expect(offer.message).toContain('could not tell whether a capture is waiting');
    // Named the way every other failure on the page is, not as a raw code.
    expect(offer.message).toContain(describeFailure(status));
    expect(offer.message).not.toContain('StorageQuotaExceeded');
    expect(offer.message).toContain('reload');
    expect(offer.message).not.toContain('enable the camera to continue');
  });

  it('does not read a store that could not be loaded as one with nothing in it', () => {
    // The worker's document store starts empty when its database will not open, so the listing
    // succeeds with nothing in it while a capture may be waiting on disk (ADR 0078).
    const offer = startOffer({ ok: true, value: [] }, 'VersionError: the database is newer', false);
    expect(offer.resume).toBeNull();
    expect(offer.message).toContain('could not tell whether a capture is waiting');
    // And what a new capture would be, as well as what it would cost: nothing is written down.
    expect(offer.message).toContain('would not be saved');
    expect(offer.message).toContain('VersionError: the database is newer');
    expect(offer.message).toContain('reload');
    expect(offer.message).not.toContain('enable the camera to continue');
  });
});
