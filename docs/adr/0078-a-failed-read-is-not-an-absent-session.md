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
  status for a read that failed, as it already does for the tier's. The page offers a resume
  again for every code but `Unsupported` and `SensorUnavailable`, which say no press changes the
  answer (ADR 0039, ADR 0044) and would mean that from a store too. For any other store failure
  this changes the words and not the behaviour.
- **The page says what a failed listing means.** The stage line names the failure, tells the user
  to reload, and says a new capture would clear one that is waiting. That is the press the line
  used to invite as "continue" (`startOffer`, `resume.ts`). The new capture stays on offer.
- **A document store that would not load is said, and never written over.** The browser's
  document host keeps the app running from memory when its database will not open, so the core
  lists nothing and fails nothing. The fold is the store's, one layer below this rule. The host
  now persists nothing after a failed load, and the worker reports the load's error at boot. The
  page then shows the failed-listing line (`DocumentHost.loadError`, `RemoteCore.storageError`).

## Consequences

- No store in this build fails a read with anything but `NotFound`, so `List`'s and `Resume`'s
  branches are reached only through test stores that fail a read on request. They exist so that a
  store that can fail does not inherit a silent fold.
- A failed load is reachable. Private browsing, blocked storage, a corrupt database, and an older
  build meeting a newer one's database (`VersionError`) all start the host empty. Before this, the
  page then invited a new capture. Its `Begin` emptied the tier, `Create` reissued project 1, and
  the host persisted the new capture's title and session over the old one's on disk. Now the disk
  is left alone, and the line says a capture may be waiting. The tier is not, since a new capture
  pressed anyway still empties it: that is the choice the line puts to the user.
- One project's unreadable session fails the whole listing, the others' with it. Today the page
  lists once at load to find one capture to offer, so nothing is lost that it used.
- A title that cannot be read still lists as an empty title. That costs a blank row and no frames.
  `Begin` still answers one as `NotFound`, "no such project", which refuses a capture and loses
  nothing.
- A refused resume still ends "try again, or start a new one", for a store failure as for every
  refusal the page offers again. There the resume stays on offer, so the waiting capture is in
  front of the user. After a failed listing nothing is, which is why only that line warns.

## Rejected alternatives

**`hasSession = true` when the read failed.** It offers the resume, which protects the frames, and
a transient failure would resume on the press, where this decision makes the user reload. Rejected
because the summary would then say a session exists when nothing said so: `hasSession` is the
fact, not a promise (ADR 0036), and a flag that is sometimes a guess is one no reader can trust.

**Withdraw the new capture after a failed listing.** It protects the frames completely, and it
locks out a user whose store fails only its session reads, until the store recovers, which a
reload may never make happen. A store that fails every read needs no withdrawing, since `Begin`
reads the title first and refuses. The line says what the press would cost and leaves the choice
with the user.
