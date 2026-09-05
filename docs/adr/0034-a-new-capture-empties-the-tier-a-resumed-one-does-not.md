# ADR 0034 — A new capture empties the spill tier; a resumed one does not

## Context

ADR 0029 gave a session a document that outlives its tab, and ADR 0030 gave the spill tier the same
property: a fixed preferred file and a sibling index carrying the frame-to-offset map, so a reload
can find the pixels the document names. Together they make a resume possible.

They also make a collision possible. Frame identities come from a counter inside
`MemoryFrameStoreAccess`, and that counter starts at 1 in every process — the store dies with its
tab. The tier does not. So the identities a new capture is about to issue are, on a device that has
captured before, exactly the identities the tier is already holding frames under.

What that produced was not an error. The spill host's allocator is exact-fit and keyed by frame id:
a `write(1, …)` against a tier that already has a frame 1 frees the old slot and takes it straight
back. The old capture's pixels are overwritten, and the old capture's session document goes on
naming frame 1 as though nothing had happened. A later `Pin` of it succeeds — with the wrong
pixels. There is no length mismatch to catch it (a phone's frames are all one size) and no fault-in
failure to report, because the bytes really are there. It is the one failure the whole
`Adopt`-then-`Pin` path cannot detect, and the reason is that nothing about it is a failure.

## Decision

**`IFrameStoreAccess` gains `Clear()`, and `CaptureSessionManager::Begin` calls it. `Resume` does
not.** A capture that is starting over says so by emptying the tier before it issues its first
identity; a capture that is coming back to a sphere is the one thing the tier survived a reload
*for*, and clearing it there would destroy exactly what it was opened to recover.

**`ISpillSink` gains a fourth call, `Clear()`.** It does not break that seam's no-listing rule
(ADR 0020). The store still never asks what is down there — it says that none of it is wanted. The
frames that make a clear necessary belong to a process that is gone, so dropping them one at a time
is not open to a store that has just started and knows of no frames at all.

**The clear happens late in `Begin`, after the plan and before tracking starts.** It is the only
irreversible thing `Begin` does, and the tier holds the only copy of whatever sphere came before. A
`Begin` that emptied the tier and then failed to plan would have destroyed a capture on its way to
doing nothing. Its own failure path closes the camera, which by then is open because the lens is an
input to the tessellation.

**A tier that will not empty stops the session from beginning, with nothing forgotten.** This runs
against the grain of the rest of the core, where a degraded device is carried: no sensor means
vision-only, no sink means no spill. Not here. Beginning anyway would put the new capture's frames
on top of a sphere that is still on disk and still named by a document, under the very identities
that document carries. Declining is recoverable and loud; capturing over it is neither. So the
store refuses before it drops anything, and "refused" and "unchanged" are the same state.

**`Clear` is refused while any frame is pinned.** `Pin` guarantees its mapping until `Release`, and
a span into a freed buffer still looks like a span.

**"Unchanged" has to hold across a reload, not only in this process.** The store keeps its entries
on a refusal by never touching them, which is easy; the tier underneath has to work for it. The
browser sink writes its emptied index before it truncates the frames file, so a failure at the
first step changes nothing at all — and a failure at the second, where the frames file is untouched
because the handle that threw never modified it, puts the old index back. Only if that restore
also fails is anything lost, and what is left then is an index naming nothing over a file that
still holds the bytes: the capture is gone, which is bad, and no identity resolves to somebody
else's pixels, which is the thing that must not happen. That asymmetry is the whole reason the
index is written first rather than last.

**`Clear` does not wind the identity counter back.** A handle is a plain value that outlives the
frame it names. Reissuing one would turn a stale handle from dangling — a `NotFound` the caller can
act on — into a handle on somebody else's pixels, which is the failure this ADR exists to remove.

## Consequences

The same-project case is now correct in both halves, and that is the case the scope note in
`docs/06-roadmap.md` settled on: in practice there is never more than one unfinished sphere,
because resuming means standing in the same spot again. `Begin` empties the tier, and the
`Checkpoint` at the end of the same call replaces that project's session document with an empty
one. There is no moment at which the paperwork names frames the tier no longer has.

Abandoned spheres stop accumulating on disk. Before this the file grew to the high-water mark of
every capture the device had ever started and never came back below it, because nothing ever
dropped a frame the process that spilled it had forgotten about.

**What this does not close: a session document belonging to a *different* project.** `Begin` clears
the tier, and the new capture then reissues identities from 1 — so another project's document, which
this manager cannot see and has no way to invalidate, still names identities that now belong to
someone else's pixels. `CaptureSessionManager` reads documents by project id through
`IProjectStoreAccess` and has no listing; inventing one so that beginning a capture could go and
edit every other project's session record would be a manager reaching across the whole store to
enforce a global invariant, which is the wrong shape for it. The narrower fix is an epoch: the tier
bumps a generation on every clear, the session document records the generation its frames were
written under, and `Resume` refuses a document whose generation has passed. That is a decision of
its own and it is recorded in `docs/06-roadmap.md` rather than taken here.

## Alternative rejected

**Clear the tier when the worker starts, rather than when a session begins.** Simpler, and it needs
no contract change at all: the composition root empties the file before it builds the store. It also
makes a resume impossible, which is the property ADR 0029 and ADR 0030 exist to provide — a reload
would find the document and nothing behind it. The roadmap's scope note already chose against this
for the same reason.

**Make identities unique across processes instead — a random or persisted prefix.** This closes the
cross-project case too, which the decision above does not, and it needs no `Clear` at all. It was
rejected as the wrong first move rather than as wrong: it puts durable, cross-session identity into
the one component whose whole job is to model a *device's* memory, it does not reclaim the disk that
abandoned spheres are sitting on, and a store that never reuses an identity still needs a way to
drop the frames nothing names. The epoch described above is the same idea confined to the tier,
where the state already is.
