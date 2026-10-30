# 0083 — The capture offered back is the newest one this page's tier holds

**Status:** accepted; amends [0036](0036-a-resumable-capture-is-visible-in-the-project-listing.md),
whose offer was the newest capture anywhere, and closes the case
[0081](0081-the-store-issues-project-ids.md) recorded as open

## Context

The page offers back the newest project with a session document (ADR 0036). Only one spill tier is
resident at a time. The tab holding it writes the tier the next load resumes from, and a tab opened
beside it captures into a tier of its own (ADR 0063). So the second tab's capture can have the
larger id. It was then offered, and `Resume` refused it for naming another tier (ADR 0035). That
refusal keeps the offer up (ADR 0039), so it came back on every load. The capture the resident tier
does hold was never offered, and the other press, a new capture, cleared it (ADR 0034).

ADR 0081 made this easier to reach: a stale tab used to overwrite the other tab's project, and now
issues an id above it instead. It recorded the case as open.

The page cannot tell the two apart. The tier a document names is in the document, and the tier
the page holds is a question for the frame store. Neither reaches the page.

## Decision

- **`ProjectSummary` says whether its session is in another tier.** `sessionInAnotherTier` is
  true when the session document names a tier token other than the one the frame store holds
  now. `ProjectManager` reads the token from the document, as `SetSelection` already reads its
  candidate counter, and asks `IFrameStoreAccess::TierGeneration` for the store's. It takes the
  frame store for that one question.
- **The page offers the newest capture not in another tier.** If every capture is in another tier,
  nothing is offered. A new capture then empties this page's tier, which holds none of them.
- **A document this build cannot read is not said to be elsewhere.** It cannot say which tier it
  names. It stays on offer, and its refusal says it is kept for a build that can read it, as
  ADR 0039 decided.
- **A tier that cannot say which capture it holds fails the listing**, when a session needs placing
  against it, with the tier's status. Answering "not elsewhere" for it would offer a capture
  `Resume` refuses, as before. A listing with no readable session does not ask the tier.
- The tier is asked on every listing, not remembered. A new capture empties it under a new token,
  and then the capture it held is the one in another tier.

## Consequences

- A capture taken in a second tab is not offered by the tab holding the resident tier, nor after a
  reload of it. It becomes resumable from a page that holds its tier. A page that reloads takes
  the resident pair back (ADR 0063), so that is the tab that held it when the capture was taken.
- Zero, a store with no tier, matches a document written without one (ADR 0035). Desktop and
  tierless browsers offer exactly what they offered before.
- **The refusal ADR 0039 kept the offer up for is no longer reached through the offer.** A
  mismatch now needs the tier to change between the listing and the press, and only `Begin`
  changes it, which hides the offer. The browser test that pressed a resume refused for its tier
  is now the test of this decision: a capture this tier holds is offered over a newer one from
  another tier, and resumes. What it also pinned — that a refusal which keeps the offer leaves it
  pressable, and that a second press retries the session without reopening the camera — is no
  longer reachable from the page in one tab, and is held by `describeResumeRefusal`'s unit tests
  alone.
- The listing now depends on the tier answering whenever any readable session exists. A page whose
  worker script is older than its module fails the listing rather than offering, and says it could
  not tell whether a capture is waiting (ADR 0078).
- `ProjectManager` decodes every session document on each listing, where it only checked that each
  existed. A listing is made once per load.

## Rejected alternatives

**Fall back on the page after a refusal for the tier.** The page would offer the newest, and on a
`FailedPrecondition` naming the tier, offer the next. It needs no contract change, but it classifies
a refusal by its wording, and each wrong capture costs a press and a camera prompt.

**Carry the token in the summary, and let the page compare.** The page does not know its own tier's
token, and giving it one is a second copy of a fact the frame store holds.

**Hide sessions in another tier from the listing.** `hasSession` would then mean two things, and a
page showing a project list could no longer say that a capture exists somewhere.
