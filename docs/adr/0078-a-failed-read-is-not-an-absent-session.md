# 0078 — A failed read is not an absent session

**Status:** accepted; amends [0036](0036-a-resumable-capture-is-visible-in-the-project-listing.md)

## Context

ADR 0036 put `hasSession` on `ProjectSummary` and filled it with whether reading the project's
`session` document succeeded. Success and existence were the same question only while every store
failed a read with `NotFound` and nothing else, which both shipped stores still do. The contract
lets a read fail any way it likes, and `GetSelection` already refuses to fold a failure into
absence for exactly that reason.

The fold matters more here than anywhere else. A summary with no session is what keeps a capture
from being offered back, and the page's other press is a new capture, whose `Begin` empties the
spill tier that capture's frames are in (ADR 0034). A read that failed once, folded into "no
session", ends with the frames erased by a press the page invited. `Resume` made the same fold the
other way, answering any failed read as `NotFound`, "no session to resume".

## Decision

- **`ProjectManager::List` reads `NotFound` as "no session" and nothing else.** Any other failure
  of a session read fails the listing with the store's status, since a summary has no way to say
  "could not tell".
- **`Resume` answers `NotFound` only for a document that is not there**, and the store's own
  status for a read that failed. The page offers a resume again for either, so this is the word
  rather than the behaviour.
- **The page says what a failed listing means.** The stage line names the failure, tells the user
  to reload, and says a new capture would clear one that is waiting. That is the press the line
  used to invite as "continue" (`startOffer`, `resume.ts`). The new capture stays on offer.

## Consequences

- No store in this build can reach either path: the browser's and the in-memory one fail a read
  only with `NotFound`. Both are reached through the test stores that fail a read on request. They
  exist so that a store that can fail does not inherit a silent fold.
- One project's unreadable session fails the whole listing, the others' with it. Today the page
  lists once at load to find one capture to offer, so nothing is lost that it used.
- A title that cannot be read still lists as an empty title. That costs a blank row and no frames.

## Rejected alternatives

**`hasSession = true` when the read failed.** It offers the resume, which protects the frames, but
the summary then says a session exists when nothing said so. A resume pressed on it fails with the
store's status anyway, so the page ends where this decision puts it, by way of a claim nobody made.

**Withdraw the new capture after a failed listing.** It protects the frames completely, and it
locks out a user whose store fails every read until the store recovers, which a reload may never
make happen. The line says what the press would cost and leaves the choice with the user.
