# ADR 0020 — Where a spilled frame goes is a seam inside the frame store, not a port beside it

**Status:** accepted

## Context
[ADR 0019](0019-the-core-runs-in-a-worker.md) moved the core into a worker so that a spilled frame
could be faulted back in synchronously, and said in one line what would fill the gap: *"Spill is a
new port over an OPFS sync access handle, opened during worker startup and held for the session."*
That line is right about the mechanism and wrong about the shape, and the difference matters
because "port" in this codebase means a contract in `contracts/cpp/sphanorama/resource_access/`
with an entry in the volatility map and an owner of its own.

`MemoryFrameStoreAccess` had reached the point where the gap was visible. Its spill tier was a
classification: `Demote` moved a number from the heap total to the spilled total and kept the
vector, so a frame that spilled freed budget without freeing memory. Everything around that was
real — the ceiling, the fault-in on `Pin`, the refusal when faulting in would overrun — and every
caller that has to cope with a frame not being resident was exercised by it. Only the destination
was missing.

So the question was where the destination belongs. Three shapes were available:

1. A second full implementation of `IFrameStoreAccess`, `OpfsFrameStoreAccess`, in `bridge/`.
2. A new resource-access contract, `ISpillAccess`, that the frame store calls.
3. A seam inside the frame-store component: one small interface, injected, with the store keeping
   every decision it already owns.

## Decision
**The third.** `ISpillSink` lives in `core/src/resource_access/frame_store_access/spill_sink.h` —
inside the component, not in `contracts/` — with three calls: `Write`, `Read`, `Drop`. The store
takes one by pointer and works exactly as before when it is null.

V11 is *where pixel bytes live*, and `IFrameStoreAccess` owns it. Residency tiers, the ceiling,
when a frame should leave the heap, what happens when faulting it in would overrun — all of that
is the store's and none of it moves. What varies underneath is one thing: the destination, RAM on
a desktop and an OPFS file on a phone. That is a single component's implementation detail rather
than a second axis, and the volatility map gains no row.

The layer rules say so independently, which is the useful part rather than a coincidence. A
resource access may not call another resource access, so shape 2 would need either a layer-rule
exception or the pretence that a spill file is a resource the *manager* orchestrates — and a
manager deciding when to spill is precisely the knowledge `IFrameStoreAccess` exists to hold. The
seam keeps the call inside one component, which is the one same-layer edge the checker allows.

Three behaviours are worth naming because they are the failure modes a phone actually produces:

- **The write happens before the heap is freed.** A sink out of quota is ordinary, and a demotion
  that dropped the bytes and then reported failure would lose a captured cell to a full disk.
- **A fault-in reads into a buffer of its own and moves it in whole.** Filling the entry in place
  would leave a frame-sized allocation full of nothing behind a failed read — memory the store
  does not charge itself for, on a device that spilled because it had none.
- **A spilled frame keeps its content hash**, taken as the bytes leave. The build graph
  fingerprints by it, so hashing the empty vector left behind would make every spilled frame
  identical to every other and an incremental rebuild would reuse one cell's work for another's.
  It cannot go stale: a spilled frame cannot be pinned, so nothing can write to it.

## Consequences
- There is one frame store, not two. The arithmetic that decides whether a sphere fits exists
  once, and the browser differs from the bench by a constructor argument rather than by a second
  implementation held equal by a contract suite.
- `MemoryFrameStoreAccess` keeps its name for now and it is already slightly wrong: the heap tier
  is memory, the spill tier is whatever the sink is. Renaming it is a mechanical change and gets
  its own commit rather than riding along with behaviour.
- `ISpillSink` is not in `contracts/`, so it is not generated, not mirrored, and not part of the
  boundary. A browser implementation of it is still browser code and still lives in `bridge/`,
  where Emscripten is allowed — under `bridge/resource_access/frame_store_access/`, which the
  layer checker reads as the same component as the store and therefore a legal edge.
- The sink is injected by the composition root, which is where the choice of implementation
  already lives (ADR 0014). Nothing above it learns that a spill destination exists.
- **With no sink there is no spill tier, and demoting to one is refused.** The classification
  behaviour described above was left in at first as the sinkless fallback, and that was wrong:
  a store that moves a frame between budget totals without moving a byte has freed room the
  machine does not have, and the next allocation takes it. Harmless on a desktop and fatal on a
  phone — but the store cannot tell which it is on, and a component whose reason to exist is
  modelling memory pressure is the last place to put a number that lies about it. So the rule is
  uniform: somewhere to put the bytes, or no spilling. The native build has no sink and therefore
  no spill tier, which is a ceiling refusal on a machine with 512 MB to spend.
- The contract suite runs against a store *with* a sink, since half the contract is about a frame
  that is not resident and a sinkless store can no longer get one there.
- A refused `Drop` is reported and the entry stays, so the budget keeps accounting for bytes the
  sink still holds. That makes a sink which persistently refuses able to prevent a `Forget`, which
  is the right trade for now — silently leaking quota is the alternative — but it is a real edge
  and it is written down rather than discovered.
- The single OPFS handle ADR 0019 describes means the browser sink owns an allocator: one file,
  offsets per frame, and a free list. That is the sink's problem entirely and none of the store's,
  which is the clearest evidence the seam is in the right place.

## Rejected alternative
**A second `IFrameStoreAccess` implementation over OPFS.** It is the shape the repo already uses
for camera, motion and the project store, and it is wrong here for a reason those do not share:
those contracts differ between platforms in *what they can do*, and this one does not. The
tiering, the ceiling and the fault-in are identical; only the destination changes. Duplicating
them would mean two copies of the arithmetic that decides whether a capture fits in memory, kept
equal by a contract suite — and the contract suite cannot check the interesting part, because
"does this ceiling model this device" is not a property two implementations can agree on.

**A new `ISpillAccess` contract.** It reads well until the layer rules are applied: a resource
access calling a resource access needs an exception, and the only way to avoid the exception is to
have a manager sequence the spill — which would put "when should a frame leave the heap" in a
manager, where it cannot see the budget and has no business deciding. The volatility map is also
evidence against it: V11 already names "OPFS spill file" as one of the places pixel bytes live, so
the axis has an owner and this is that owner's implementation.
