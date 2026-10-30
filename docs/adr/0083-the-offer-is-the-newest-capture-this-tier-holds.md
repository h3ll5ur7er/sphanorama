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
- **Zero matches zero only while the document names no frame.** A store with no tier answers
  zero, and a document written on one records it (ADR 0035). But zero does not vouch for an
  identity, and such a store adopts no frame, so a document that names one is elsewhere: `Resume`
  would refuse it, as `PanoramaBuildManager` refuses zero (ADR 0070).
- **And whether its document is one this build cannot read.** `sessionUnreadable` is true there.
  Such a document cannot say which tier it names, so it is not said to be elsewhere.
- **The page offers the newest readable capture not in another tier, and an unreadable one only
  where there is none.** Offered first, an unreadable capture is refused as `Unsupported`, the
  offer is withdrawn (ADR 0039), and the press left is a new capture, which empties the tier a
  readable capture is in. Offered alone, its refusal says it is kept for a build that can read it.
  If every capture is in another tier, nothing is offered, and a new capture empties this page's
  tier, which holds none of them.
- **A tier that cannot say which capture it holds fails the listing**, when a session needs placing
  against it, with the tier's status. Answering "not elsewhere" for it would offer a capture
  `Resume` refuses, as before. A listing with no readable session does not ask the tier.
- The tier is asked on every listing, not remembered. A new capture empties it under a new token,
  and then the capture it held is the one in another tier.

## Consequences

- **A capture taken in a second tab is never resumable, and now is never offered.** Its tier is a
  fallback pair under a name of its own, removed when that tab closes and swept if it survives,
  and that tab's own reload opens another under a new token (ADR 0063: a fallback pair is nobody's
  to resume). So its project keeps a session in another tier for good. Showing those projects as
  lost, or letting a user delete them, is not done here.
- A store with no tier offers what it offered before for documents written without one, and no
  longer offers one naming frames it cannot adopt. A page that fell back to no tier, a second tab
  whose fallback could not be opened, now offers nothing where it offered a capture written with a
  real token and was refused for it.
- **A page that loses the race for the resident tier is not offered the capture it holds.** ADR
  0063 accepts two such pages: a tab opened within about two seconds of the app's last tab
  closing, and a reload whose old worker holds the files past its wait. Each takes a tier of its
  own, so the resident tier's capture reads as elsewhere, while it is still on disk and a reload
  offers it. Before this, it was offered and refused for its tier. Such a page knows it holds a
  tier of its own, and says a capture is waiting that a reload with the other tab closed can
  resume. On the resident tier nothing is said, since a capture in another tier there is a
  fallback tier's.
- **A tier mismatch is no longer reached through the offer.** It now needs the tier to change
  between the listing and the press, and only `Begin` changes it, which hides the offer. A resume
  can still be refused after it was offered for reasons a press could clear, the store or the
  sensor, so ADR 0039's offer-again path stands. The browser test that reached it through a tier
  mismatch now reaches it through a refusal injected at the core the page calls, and a second
  browser test offers and resumes a capture this tier holds over a newer one from another tier.
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
