# ADR 0023 — A committed cell is cooled by the session, not evicted by the store

**Status:** accepted

## Context
The spill tier has been a complete mechanism with nothing above it. `MemoryFrameStoreAccess` has
residency tiers, a sink to put bytes in ([ADR 0020](0020-the-spill-destination-is-a-seam-inside-the-frame-store.md)),
a fault-in on `Pin` that re-checks the ceiling, and — since the ceiling stopped being a number
written in the source — a real figure underneath it, read from `navigator.deviceMemory`. What it
did not have was anything that ever called `Demote`. `Allocate` refused at the ceiling, no
production caller demoted, and the roadmap said so in as many words.

The arithmetic makes the consequence concrete rather than theoretical. A preview frame capped at
1280 on its long edge is 1280×960×4 ≈ 4.9 MB, `BurstSpec` keeps five per cell, so a cell is about
25 MB. A phone-FoV tessellation is a couple of dozen cells. That is several hundred megabytes
against a ceiling of 128 MB on a mid-range phone: the capture stops partway round, and the user's
recourse is to start again and stop at the same place. Phase 1's exit criterion is a full 360×180
capture that completes without an OOM, and it was unreachable by construction.

So the question was never whether frames should leave the heap. It was **who decides when**.

## Decision
**`CaptureSessionManager` cools a cell on every way out of a burst** — inside `Disarm`, which is
the one place every path already goes through: completion, a failed score or rank, a retake, the
end of a session. Every candidate of that cell *that a burst here produced*, not only the frames
this burst took, is offered to the store as `Demote(frame, Residency::Spilled)`.

Both halves of that sentence are corrections of a first attempt that cooled on completion only,
and cooled the cell wholesale:

- **On completion only leaks.** Scoring a retake reads its siblings, which faults the cell's
  spilled frames back into the heap; a burst that then ends any other way leaves them resident for
  the rest of the session — the ceiling this exists to keep clear, held by the one route that
  skipped the policy.
- **The cell is not an ownership boundary.** `OfferFrame` appends the caller's candidates to the
  same vector, and their frames are the caller's handles. Demoting one sends a frame somebody else
  is holding to a sink they do not know exists, so their next `Pin` faults from it or is refused at
  a ceiling they cannot see. The manager already says exactly this about its rollback mark, one
  line further up the same function; the ids of burst-produced candidates are now tracked so the
  boundary is a fact rather than a hope.

The moment is chosen because it is knowable there and nowhere else. A ranked cell is finished:
nothing reads its pixels again until the build or the review client asks, and both of those go
through `Pin`, which faults them back in. That is a fact about the *sequence of a session*, which
is V1 and the manager's own axis — not a guess about which frames look least recently used.

What the manager does not decide is anything about the tier. Whether there is somewhere cheaper to
put a frame, what cheaper costs, what happens when faulting it back in would overrun the ceiling,
and what the ceiling is — all of that stays in `IFrameStoreAccess` exactly as
[ADR 0020](0020-the-spill-destination-is-a-seam-inside-the-frame-store.md) left it. The manager
names a moment; the store answers what it can do with it.

**This refines a sentence in ADR 0020**, which is why this is an ADR rather than a commit. That
one says:

> a manager deciding when to spill is precisely the knowledge `IFrameStoreAccess` exists to hold

read in context, that was an argument against shape 2 — a manager orchestrating the spill *file*
as a resource, which would have made the destination a manager's business. That conclusion stands
and the sink is still a seam inside the store. But the sentence is broader than the argument under
it, and taken literally it forbids the only component that knows when a cell is finished from
saying so. The split this ADR draws is: **the session owns the moment, the store owns the
mechanism and the refusal.**

**A refusal is not reported.** `Cool` discards the status from every `Demote`, deliberately:

- A store with **no sink** answers `Unsupported`. That is the native build and any browser whose
  OPFS handle did not open — a supported configuration that caps a sphere at what fits in RAM,
  which the capture client says out loud, rather than a fault.
- A sink that **refuses the write** is a phone out of quota. The store leaves the frame exactly
  where it was: still in the heap, still readable, still this cell's evidence.

In both cases the cell is already captured by the time this runs, so there is nothing to fail.

What discarding the status costs is the **cause**, and that is worth being exact about. The
consequence is not lost: the heap keeps bytes it hoped to give back, and the next allocation that
does not fit is refused. But `FrameStoreExhausted` says the ceiling was reached and nothing more —
a sink out of quota and a capture genuinely too large for the device look identical from there.
Carrying the reason that far would mean the store remembering that a spill was refused and saying
so when it later runs out, which it does not do today. That is a diagnostic worth adding and it is
named in the consequences below rather than assumed away here.

## Consequences
- A sphere larger than the store is capturable. Peak heap during capture is one burst plus the
  frames a retake faults in to score against, rather than everything captured so far.
- **The whole cell is cooled, not the burst.** Scoring a frame against its siblings reads their
  pixels, so a retake faults the earlier burst's frames back into the heap; cooling only the new
  ones would leave those resident for the rest of the session — the failure this exists to
  prevent, arriving through the door marked "already handled".
- **`OfferFrame` is not cooled**, and this is enforced rather than asserted. Its frames belong to
  the caller (file import, replayed datasets, a manual shutter), and changing the residency of a
  borrowed handle is a surprise the borrower has no way to expect. A session driven entirely
  through `OfferFrame` — the bench — therefore gets no spilling and is bounded by its ceiling,
  which on a desktop is what it wants anyway.
- **`RequestRetake(replace = true)` still forgets offered frames**, which is the same ownership
  question answered the other way and predates this. It is defensible — a caller asking to replace
  a cell's evidence is asking for exactly that — but it is worth knowing that the two paths do not
  agree about whose frames those are.
- The store's refusal at the ceiling is still there and is still the backstop. It is now what it
  should always have been: the report of a capture that genuinely does not fit, rather than the
  first thing a normal capture runs into.
- Nothing moves in the volatility map. V1 gains a decision it already had the knowledge for, and
  V11 keeps everything it owned.
- **A capture that stops because the sink was refusing cannot be told from one that was simply too
  large.** Both end at `FrameStoreExhausted` naming the ceiling. The fix is small and belongs to
  the store rather than here — remember that a spill was refused, and say so in the detail of the
  refusal that follows — and it is worth doing before anyone has to debug a phone from a
  screenshot of the error.

## Rejected alternative
**Evict inside `Allocate`.** When an allocation would overrun the ceiling, demote resting frames
until it fits, and refuse only if that is not enough. It is the conventional answer, it needs no
caller to cooperate, and it would work for the bench too.

It was rejected because the store would be guessing. It has no idea what a cell is or when one is
finished; the best rule available to it is least-recently-pinned, which is a proxy for coldness —
and the manager has the real answer for free. Given both, the proxy runs only when the manager is
failing to do its job, which makes it a way of not noticing that. The refusal is more useful than
a silent recovery there.

The two are not exclusive and eviction may still earn its place — a review client faulting a
sphere's frames back in to display them is a caller with no natural "finished" moment, and that is
the case that would justify it. It should be added when that caller exists and can be measured,
not in advance of it.
