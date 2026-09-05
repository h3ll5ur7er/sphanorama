# ADR 0035 — The spill tier says which capture it is holding, and a resume checks

## Context

ADR 0034 gave `Begin` a `Clear`, so a new capture no longer inherits the previous one's slots. It
also named, and declined to take, the decision this ADR is: the case of a session document
belonging to a *different* project.

The mechanism is the same one ADR 0034 is about, one step along. Frame identities come from a
counter inside `MemoryFrameStoreAccess` that starts at 1 in every process, because the store dies
with its tab. The tier does not (ADR 0030). So a `Begin` empties the tier and then reissues
identities from 1 into it — and a document belonging to some other project still names those
identities. The same-project case is closed by construction, because the `Checkpoint` at the end
of `Begin` replaces that project's document in the same call. Nothing does that for anybody else's:
`CaptureSessionManager` reads documents by project id through `IProjectStoreAccess`, which has no
listing, and inventing one so that beginning a capture could go and edit every other project's
record would be a manager reaching across the whole store to enforce a global invariant.

What that leaves is the one failure the `Adopt`-then-`Pin` path cannot detect. `Resume` on the
stale project adopts frame 1, pins it, and gets the *new* capture's pixels: the right length, since
a phone's frames are all one size, no fault-in error, since the bytes really are there, and no
mismatch anywhere for anything to report. It succeeds, with the wrong image, and every check in
the system agrees that it worked.

The narrower fix ADR 0034 pointed at is an epoch: the tier says which capture it is holding, the
document records what it said, and a resume compares them.

## Decision

**`IFrameStoreAccess` gains `TierGeneration()`, and `ISpillSink` gains `Generation()` behind it.**
The token lives in the tier's own index — `StoredIndex` in `shell/src/access/spill-host.ts`, which
is already the only state of that component that survives the process — and surfaces through the
seam that is already there. `ISpillSink` is a seam inside the frame-store component rather than a
contract of its own (ADR 0020), so the fifth call needs no new port; `IFrameStoreAccess` is a
contract, and this ADR is what a change to one costs.

It does not break the sink's no-listing rule, for the same reason `Clear` does not: the sink says
which capture it holds, never *what* it holds.

**A token, minted fresh, compared for equality — not a counter compared for order.** ADR 0034
described "a generation whose generation has passed", and ordering turns out to be the wrong shape.
An unreadable index starts the tier empty rather than throwing (ADR 0030) — a tab killed mid-write,
a quota exhausted between the frame and the index — and a counter that lost its index would have to
restart from 1, reissuing an epoch a surviving document may already name. That is this ADR's own
failure, one level up: an identity that restarts while the thing it names does not. So the browser
mints 52 bits out of `crypto.getRandomValues` on every clear and whenever it opens without a
readable token, and nothing anywhere asks which of two tokens is newer.

**Zero means there is no durable tier at all**, and it is an answer rather than a refusal. A store
with no sink — a desktop, or a browser with no origin private file system — has no pixels that can
outlive it, so there is no capture there for another project's document to be matched against
wrongly. Such a store reports zero, a document written against it records zero, and a resume on
another such host matches. Refusing every resume on those hosts would take the feature away for a
hazard they do not have; inventing a token for them would claim a durable tier that does not exist,
and the frames the document names are refused by `Adopt` anyway — which is where the honest failure
for a tierless store already is, and it names the tier rather than the epoch.

The two directions across that boundary both refuse, which is what makes zero safe to match: a
document written on a host with a tier (non-zero) does not match a host without one, and a document
written without one does not match a tier that has since appeared.

**A tier that is present and cannot say is refused, not guessed at.** The browser's sink is a call
into a page-side object, and a page can be half-updated — a service worker serving a cached worker
script beside a fresh `.wasm`, which is the case `Clear`'s own guard already exists for. The host
call answers with a negative number there, which is not a token; the sink turns it into
`Unsupported`, and the two callers above it do opposite-looking things for the same reason.
`Checkpoint` writes no document at all, because a document whose token was invented is one a later
resume could match. `Resume` refuses, because it cannot tell whether the frames in front of it are
the ones the document names. The capture in front of the user carries on either way: what is lost
is the resume, and only while the page is in that state.

**The session document's version goes to 2, and version-1 documents stop loading.** The tier line
is required, like the session, lens and spec lines — a missing line must not read as a token of
zero, or a document that does not say would resume on exactly the hosts that cannot check anything.
Every line is already checked with `exhausted()`, so an older build refuses a new document too. The
documents that stop loading are precisely the ones that cannot say which capture they belong to,
which is the same judgement this ADR makes about every other unprovable document — but it is a
capture somebody may still be able to see on their phone, so it is said here rather than left to be
discovered. `INDEX_VERSION` goes to 2 with it and for the same reason: an index without a token
cannot say whose frames it holds, so it is refused like any other unreadable one and the tier
starts empty under a token of its own.

**A refused document is kept, and the project is not marked unresumable.** ADR 0029 kept an
unreadable document because the bytes it names may still be in the sink and a build that
understands its shape may come along. The reasoning here is different and lands in the same place:
a mismatch is a statement about *this* tier, not about those pixels. A session that could not take
the resident pair falls back to a tier of its own (ADR 0030) and says exactly this about a resident
tier that still holds its frames — so deleting on a mismatch would turn a second tab into a
permanently lost capture. Nothing offers a project as resumable yet in any case: the page's resume
flow is still the open item on that line in `docs/06-roadmap.md`, and when it arrives, what it
needs is a project that stops being offered rather than a document that has been destroyed.

**The store asks the sink every time rather than caching.** The token changes underneath a store
exactly once — a clear that store made — but the truth of it lives in a file that outlives the
process, and a copy in memory would be the second place for it to live.

## Consequences

- **The failure ADR 0034 named is closed.** A document from another project is refused by the
  cheapest check in `Resume`, before the plan, before the frames are adopted, and before the camera
  is opened.
- **`Resume` has a new way to fail that is nobody's fault.** `FailedPrecondition`, saying the
  session was captured into a tier this device no longer holds. It is not `Unsupported`, which
  means "this build cannot read that", because a mismatch can stop being true: the next run that
  gets the resident pair may resume the very document this one refused.
- **A resume of the tier the capture went into still works**, which is the property that has to
  survive a change like this and is asserted end to end in a browser rather than only against
  fakes: after a reload, a resume of that project runs the whole way and stops at the camera, which
  is the first thing it cannot have on a page that has not opened one.
- **Every session document written before this change is refused.** They name frames that cannot be
  proven to be theirs, so the refusal is honest, but a device with an unfinished sphere on it loses
  the ability to come back to it once.
- **The tier is now the only thing that says which capture it holds**, and if its index is lost the
  answer is a fresh token rather than a wrong one — a tier whose map is gone is one nothing could
  resume out of anyway, and the token makes the refusal explicit instead of leaving it to a
  fault-in that finds no slot.
- **`ISpillSink` is five calls**, three of them about a frame and two about the tier. The
  invariant that mattered is intact: the store still never asks what is down there.

## Rejected alternative

**Make identities unique across processes instead — a random or persisted prefix on the frame id.**
This is ADR 0034's second rejected alternative, and it closes this case too: if no two captures ever
share an identity, a stale document's frames are simply not found. It was rejected there as the
wrong first move and it stays rejected here for the same reason — it puts durable, cross-session
identity into the component whose whole job is to model a *device's* memory, and every frame handle
in the system grows a field that only this one case reads. The token is the same idea confined to
the tier, where the durable state already is, and it is one number for a whole capture rather than
one per frame.

**Have `Begin` invalidate every other project's session document.** The direct fix for the case,
and the one the shape of the code argues against: `IProjectStoreAccess` has no listing, and giving
it one so that beginning a capture could rewrite every other project's record would make a manager
responsible for a global invariant across a store it otherwise reads one document at a time from.
It also fails at the moment it is most needed — a document written by a build that is not this one,
or a project store that is mid-migration, is exactly what a sweep would miss. Comparing at read
time needs no sweep to have run.

**Put the token on the session rather than on the tier** — the manager mints one at `Begin` and
writes it into both the document and the index. It removes `TierGeneration` from the contract, and
it puts the answer to "whose pixels are these" in the component that has no idea where pixels live.
The tier is what survives the process, so the tier is what has to be able to say; a manager that
minted it would be writing down a claim it cannot check on the way back in.
