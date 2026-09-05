# ADR 0030 — The spill tier has a resident name and carries its own index

## Context

ADR 0029 gave the core a resume: a session document names the frames a capture took, and
`IFrameStoreAccess::Adopt` hands those identities back to a store that never allocated them, so
the first `Pin` faults them in from the sink. On a phone that sink is an OPFS file, and neither
half of what a fault-in needs survived a reload.

The map from frame identity to a byte range lived in the spill host's memory and nowhere else. And
the file it describes was named `sphanorama-spill-<uuid>`, fresh per session, with every other
spill file swept at open — a rule ADR 0020 put in for a good reason: a sync access handle is
exclusive, so with one shared name the second tab open on the app got `NoModificationAllowedError`
and captured with no spill tier at all, for no reason it could state.

So a resumed session asked for frames whose bytes were in a file that had already been deleted,
and would not have been able to find them in it anyway. The comment in `spill-host.ts` said as
much: "resuming a capture across a reload restores the session document, not the pixel heap". That
is no longer good enough — a coverage map restored without its pixels is a capture that says
28 of 28 and builds into nothing.

## Decision

**The tier is a pair of files, and the preferred name is fixed.** `sphanorama-spill-resident`
holds the frames; `sphanorama-spill-resident.index` holds the allocator's state. A session takes
that pair when it can.

**A session that cannot have it falls back to a name of its own**, exactly as before. This is what
keeps ADR 0020's property: a second tab, or a reload whose previous worker has not been torn down
yet, still gets a spill tier and still captures a full sphere. What it does not get is a
*resumable* tier — which is correct, because the capture it would resume belongs to whoever is
holding the resident pair.

**The index is a second file, not a header.** The frame file is addressed by offset; a header
inside it would move every frame in it. It is rewritten whole on each change — at one entry per
spilled frame that is a few kilobytes against megabyte frames, and a journal would need recovery
logic of its own for state that is already recoverable by being written again.

It carries the free list as well as the slots. The holes are derivable from the slots in
principle — a gap below the high-water mark is free — but only for frames of one size: two dropped
frames side by side read as one gap of twice the length, which this allocator's exact-fit list
would then offer to nothing. Writing them down is shorter than the arithmetic and cannot disagree
with it.

**Where the index disagrees with itself, the slots win.** The high-water mark and the slots are
two statements about the same file, and only a slot is ever acted on — a read goes to the offset
it names. So the mark is raised to cover every slot on the way in. One that had fallen behind
would hand the next spill space a restored frame is sitting in, and that read would still succeed.

**A failed write reaches the index too.** A rewrite of the same size lands in the same place —
that is the exact-fit free list working — so a write that fails partway has already overwritten
the frame that was there, and the allocator's answer is to forget that frame entirely. That was
sound while the map lived only in the process: nothing could ask for it again, because the store
keeps a frame it could not spill in the heap. A durable index is precisely something that asks
again, so the forgetting has to be written down; otherwise the next session reads that slot, gets
half of each frame, and reports success.

**An unreadable index starts the tier empty rather than throwing.** This runs while the worker is
booting, and the file is on a phone: a tab killed mid-write, a quota exhausted between the frame
and the index. Losing the map costs a resume; throwing costs the session.

**The sweep never touches the resident pair, whatever its lock says.** Every other spill file is
asked for and the ones that refuse are kept — a live session's handle is its own liveness test.
The resident pair is exempt because it is *meant* to be lying unlocked: that is what a session
that ended looks like. The narrow race is the expensive one — a session that could not take the
pair falls back, and the tab that was holding it closes before the sweep runs, so the sweep meets
a finished capture wearing an abandoned file's clothes.

**A resident tier is not deleted when it closes.** A fallback tier still is: it is nobody's to
resume, so it goes with the tab that made it.

## Consequences

- **A reload can find its frames.** The bytes are in a file with a known name, and the index says
  where in it each identity sits. This is the half ADR 0029 was written against.
- **The high-water mark comes back with the slots**, which is the subtler half. An allocator that
  recovered the offsets and started `end` at zero would hand the resumed session's first spill the
  space a restored frame is sitting in — and the read would still succeed, with the wrong pixels.
- **ADR 0020's rule is narrowed, not reversed.** A name per session is still what a session gets
  when it cannot have the resident one, and the second tab's tier still works.
- **Nothing clears the tier yet, and that has to land before the page can resume.** Frame
  identities start again at 1 in a new session, so a new capture's first spill overwrites the old
  frame 1 — while some project's session document is still naming it. Today that is harmless
  because nothing in the page calls `Resume`; the moment it does, a new capture started after an
  old one would let a resume read pixels that belong to a different sphere. The fix is a `Begin`
  that empties the tier and a `Resume` that does not, which is the core's to say because only it
  knows the difference. **This ordering is a correctness requirement, not a preference.**
- **The resident file keeps the size of the largest capture** until something clears it. That is
  the cost of it being the thing a reload comes back to.
- **The fallback is for a tier somebody holds, not for a browser that cannot lock at all.** Those
  want opposite answers: another name gets you a tier in the first case and, in the second, an
  empty file that fails again and stays there — once per boot, collected by nothing, since the
  sweep only runs once a tier has been locked. "No synchronous access handles" is its own error
  type so that it stops rather than retries.
- **A tier whose index will not lock falls back too**, rather than costing the session its spill
  entirely. Two files means two locks, and only one of them has to be unavailable; giving up over
  a file that holds no pixels would cap the sphere at RAM.
- **`SpillFile` grows a `size()`**, which is `FileSystemSyncAccessHandle.getSize`. The index needs
  to know how much to read back; the frame file never asks.

## Rejected alternative

**Keep the name per session and record it in the project document**, so a resume knows which file
to open. It preserves ADR 0020 untouched and it works — but the name has to reach the worker
before the store is built, and the store is built at boot, before any project is chosen. That
means either deferring the store's construction until a session begins, or a protocol message that
hands a filename to a component whose whole point is that nothing above it knows where bytes go.
The resident name needs neither: the tier is a property of the origin, and which capture is in it
is a question the session document already answers.

**Derive the offsets from the file itself** — a length-prefixed record per frame, walked at open.
No second file, and self-describing. It also means a frame's bytes can no longer be rewritten in
place at a different size without moving everything after it, and a torn final record makes the
whole walk ambiguous in a way a JSON parse failure is not. The index is smaller than the problem
it would create.
