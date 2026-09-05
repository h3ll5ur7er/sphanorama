# ADR 0036 — A resumable capture is visible in the project listing

## Context

ADR 0029 built the core half of resume and ADR 0030 the tier under it; ADR 0034 stopped a new
capture from taking the frames a resume needs. What none of them gave the page is a way to *know*.
`shell/src/main.ts` created a project and began a session unconditionally on every enable, so a
capture interrupted by a phone call was, from the page's point of view, gone. Both halves of the
machinery were done and neither was wired to a button.

The button needs an input, and the obvious one is wrong. The page cannot find out whether there is
a session by attempting `ICaptureSessionManager::Resume`: a *failed* attempt is cheap — the
document is read before anything is touched — but a *successful* one opens the camera, starts
tracking, and leaves a live session behind. Probing would mean committing to a resume the user has
not asked for and cannot yet decline, on a device where opening the camera is the visible act that
lights an indicator.

So something has to report that a capture is resumable without doing any of it.

## Decision

**`ProjectSummary` gains `hasSession`, filled in by `IProjectManager::List()` from the presence of
the project's `session` document.** The page lists projects at load, offers the newest one that has
a session, and starts a resume only when that offer is pressed.

**The flag is the fact, not a promise.** It says a session document exists. It does not say
`Resume` will succeed — the document may be from a shape this build cannot read, the stored spec
may no longer plan, the tier may have lost the frames — and a name like `resumable` would have
promised the second. Everything stronger than "there is something here" requires parsing the
document, which is the one thing this manager must not do.

**`ProjectManager` reads a key, never a format.** It asks the store whether `session` exists and
stops there; what is inside belongs to the manager that wrote it. That is the shape the project
store already has in the other direction: `CaptureSessionManager::Begin` reads `title`, a key
`ProjectManager` owns, to check the project exists before it opens a camera. The store's key
namespace is shared and flat; the documents in it are private to their writers.

**The page offers the newest project that has a session, by identity.** Ids ascend with creation —
`Create` takes one past the highest the store holds — while the listing's *order* is whatever the
document host's key map iterates, so identity is what says which is newer. Newest rather than any,
because `Begin` empties the spill tier (ADR 0034): only the most recent session's frames are still
down there. This is also the case the feature is for, decided with the maintainer rather than
assumed — resuming means standing in the same spot again, so it is "a call came in mid-capture",
not "come back to it tomorrow", and there is never more than one unfinished sphere in practice.

**A refused resume says why and leaves a new capture one press away.** The reason goes on the stage
line through the shell's existing `describeFailure`, named as a resume so the same status arriving
from a fresh capture is not confused with it, and a "start a new capture" button appears. The
render loop is deliberately not started on that path: it is started once, by whichever call settles
the session, so a fresh start from here cannot leave two loops running over one session.

## Does this reopen ADR 0029?

It is the obvious objection, since 0029 moved `Resume` *off* `IProjectManager` — and the answer is
no, for the reason 0029 gave.

0029's argument was about what `Resume` *returns*: a `SessionId` naming a live session, whose plan,
per-cell candidate sets and pose are `CaptureSessionManager`'s state. `ProjectManager` had no way
to make one, so the method had returned `Unsupported` since it was written. The rejected
alternative there — have the client read the document through `ProjectManager` and hand it to
`Begin` — failed because the *contents* would have to cross the boundary twice, in a type invented
for the trip, so that a client could hand a manager back its own private state, and because the
client would then be the thing deciding what a malformed document means.

None of that is what `hasSession` does. It carries no session, no `SessionId`, no candidates, no
plan and no bytes of the document; nothing decides what a malformed document means, because nothing
opens it. A listing that reports one bit about a project cannot hand a session to anyone, and the
one component that can still is the one 0029 named. The two answers even come from different
questions: "is there something to come back to" is metadata about a project, which is what `List`
is for, and "give me that session back" is a live capture, which is what `Resume` is for.

The precedent is in the type already. `ProjectSummary` carries `hasBuild` — the same shape of
question about the same kind of artefact, answered by the same listing, and nobody has ever read
it as the build pipeline moving into `ProjectManager`. `hasSession` is that question about a
session document.

Honest caveat: `hasBuild` and `nodesSatisfied` are declared and never filled — `List` sets only
`id`, `title` and now `hasSession`. So the precedent is a shape rather than a working example, and
ADR 0029's "`List` still reports `nodesSatisfied`" describes an intent the code has not caught up
with. `hasSession` is the first of those fields with an implementation behind it.

## Consequences

- **A capture survives the tab, all the way to the user.** The last thing missing from the reload
  path in ADR 0029's own consequences — "the page's resume flow" — is in.
- **No new round trip.** The page already called `project.list()` at load to prove the facade
  works; that same listing now answers the resume question, so the offer costs nothing it was not
  already paying.
- **One document read per project, per listing.** Asked of the store every time rather than
  remembered: a session document appears while the manager is alive, so a cached answer would
  report the tab's own capture as unresumable for as long as that tab stayed open — and it is the
  tab holding the phone.

  It reads the document to learn only that it is there, because `IProjectStoreAccess` has no
  existence verb — `ReadDocument` is what there is, and `Exists` already uses it the same way for
  the title. That copies a full sphere's session document (a 28-cell capture is roughly 35 KB) into
  the heap per project, once per listing, and the page lists once at load. Cheap enough to leave
  alone; a `HasDocument` on the store is the fix if a project picker ever makes the listing hot.
- **Two managers now agree on a key.** `ProjectManager` looks for `session`, `CaptureSessionManager`
  writes it, and nothing in either component's own suite would notice if one of them moved. The
  test that would is a cross-manager one — capture a cell through the session manager, then assert
  the listing offers that project back — and it exists for exactly that reason.
- **The offer is only as good as the tier's honesty.** Resuming the newest session is what keeps
  the offer pointing at frames that are really there, and it holds because `Begin` empties the tier.
  The gap ADR 0034 leaves open is unaffected and unclosed by this: a session document belonging to
  a *different* project can still name frames a later capture reissued, and a page that offered
  that project would restore a coverage map over somebody else's pixels. Offering only the newest
  narrows it; a tier generation is what closes it.
- **`ProjectSummary`'s wire format changed.** One more boolean, both sides regenerated from the
  same parse (ADR 0013), so nothing can disagree about it.

## Rejected alternative

**Put the query on `ICaptureSessionManager` — `CanResume(ProjectId)`, or an `Inspect` returning an
outline of the stored session.** It has a real argument behind it: the document belongs to that
manager, so asking it is asking the owner, and it could answer more than "there is one" — it could
parse, and report that this build cannot read the document *before* the user is offered anything.

It was rejected on three counts. It is a call per project where the listing is one call for all of
them, and the page has to scan projects it did not create to find the one to offer. It puts a
second "can this be read" path beside `Resume`'s own, which is precisely the pair that drifts:
whatever `Resume` learns to refuse next has to be taught to the query too, or the page offers
resumes the core will decline. And it buys nothing the page needs, because the page must handle a
refusal anyway — `Resume` is free to fail for reasons no query can see, such as a sink that lost
the bytes it was never able to list (ADR 0029). Given a refusal path that has to exist, the cheaper
question is the right one.

**Have the page try `Resume` and undo it if the user declines.** No undo exists that is safe. The
only way back out of a live session is `End`, which checkpoints and disarms, and by then the camera
is open and the frames are adopted. It would also mean asking for camera permission on page load,
before any gesture — which iOS refuses outright and which every user reads, correctly, as the app
watching them.

**Show every project with a session and let the user choose.** A project picker is a screen this
app does not have, and the scope note above says what it would be for: there is one unfinished
sphere, and the older ones' frames are not in the tier to come back to. Offering them would be
offering coverage maps over pixels that are gone.
