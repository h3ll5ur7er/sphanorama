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
  it('says what the core said rather than a sentence about https', () => {
    // `Unsupported` is the core's word for "this build does not read that shape" and the camera
    // adapter's word for "this page is not on a secure origin". A refusal rewritten into the
    // second would send someone after a certificate for a document that simply cannot be parsed.
    const message = describeResumeRefusal({
      code: 'Unsupported',
      component: 'CaptureSessionManager',
      detail: "this project's session document is from a shape this build cannot read",
    });
    expect(message).toContain('shape this build cannot read');
    expect(message).not.toMatch(/https/i);
  });

  it('names the resume, so the reason is not read as a failure of whatever comes next', () => {
    const message = describeResumeRefusal({
      code: 'FrameStoreExhausted', component: 'MemoryFrameStoreAccess', detail: 'no room',
    });
    expect(message).toMatch(/resume/i);
    // And still the sentence the shell already gives that code, rather than a second wording of
    // it that would drift from the first.
    expect(message).toMatch(/memory for frames/i);
  });
});
