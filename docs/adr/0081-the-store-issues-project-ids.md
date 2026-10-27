# 0081 — The store issues project ids, from the clock

**Status:** accepted; amends [0036](0036-a-resumable-capture-is-visible-in-the-project-listing.md),
whose newest capture is still the largest id, and closes the overwrite
[0078](0078-a-failed-read-is-not-an-absent-session.md) left open

## Context

`ProjectManager::Create` issued one past the highest id the store listed. In the browser the store
is the document host, which loads IndexedDB once and keeps the documents resident (ADR 0014), so
the listing is what the page loaded. A tab that loaded before another tab created a project, or a
page restored from the back/forward cache, issued the other's id again. A write creates on demand,
so its title, and its session when a capture began, went over the other project's documents on
disk. An end-to-end test does exactly that: two tabs load, the second creates a project, the first
creates one, and after a reload only the first's title is listed.

Neither tab can see the other's writes when it creates. IndexedDB could say, but asking is
asynchronous, and the store's contract is synchronous so that storage's volatility does not climb
into every manager (ADR 0014).

## Decision

- **The store issues the id.** `IProjectStoreAccess::IssueProjectId` answers an id above every one
  that store has issued or held, removed or not, and creates nothing. `Create` takes it and writes
  the title under it. The store owns this because the storage is the only thing every page shares.
- **The browser's id is the clock's second over twenty random bits**, from the document host:
  seconds since the epoch times 2^20, plus a draw from `crypto.getRandomValues`, and never at or
  below the highest id the host has loaded, written or issued. A tab that creates in a later second
  than another issues a larger id whatever either holds. Two tabs creating in the same second
  collide only on equal draws, one chance in 1,048,576.
- **Ids still ascend with creation**, which is how the page picks the capture to offer back
  (ADR 0036). Ids an older build counted out from 1 are below every id the clock gives.
- **Past what a double holds exactly, no id is issued.** An id crosses to the page as a number, and
  past 2^53 two ids are one number, which is the overwrite again. A clock that far out issues ids
  from the highest held instead; a store holding an id that high refuses with
  `FailedPrecondition`. The memory store counts, since one process holds it.

## Consequences

- An id is about 2^50.7 today. It stays under 2^52 until 2106 and under 2^53 until 2242.
- Uniqueness across tabs is a probability, not a check. Besides an equal draw in one second, a clock
  set back can make a tab issue an id another tab issued earlier; the floor keeps one tab's own ids
  apart, not two tabs'. A refusal at persist time would be the check, and it would come after the
  core has written under the id.
- Ids are no longer small. Nothing displays one, and every reader takes an id as a double and checks
  it whole and in range (`wire::IsRepresentableId`) before using it.
- The host still persists nothing after a failed load (ADR 0078). The reason given there, ids
  restarting at 1, is gone, but a store that would not load may hold captures a delete or a new
  capture's tier clear could still cost. That guard is not revisited here.
- A worker older than the module has no `issueProjectId`. The bridge answers that as
  `FailedPrecondition`, so `Create` fails rather than counting.

## Rejected alternatives

**A store that refuses to create over an existing project.** It keeps ids small, but the only thing
that can see another tab's project is IndexedDB, and a synchronous call cannot ask it. The host
could refuse only when it persists, after the core has already written the title and possibly a
session under the id, and nothing above the host could then be told which id to move them to.

**One capturing tab, by a Web Lock.** It blocks the second tab instead of letting it work, and a
tab that takes the lock after another let it go still holds the listing it loaded before that tab
created anything.

**Fully random ids.** 52 random bits make a collision rarer still, but "newest" is the largest id,
and a random one says nothing about when it was issued. Ordering would then need a creation time
stored in a document, which is a second copy of what the id already says.
