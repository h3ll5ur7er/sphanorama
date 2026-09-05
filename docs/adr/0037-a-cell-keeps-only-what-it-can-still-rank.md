# ADR 0037 — A cell keeps only as many candidates as it can still rank

## Context

Retakes accumulate. A second burst on a cell is appended to the first and the whole set is ranked
together, which is the feature working: the best of everything shot at that direction wins, and a
retake competes rather than replaces. Nothing bounded the accumulation.

Ranking is what makes that expensive. Scoring reads every candidate's pixels, so `Rank` faults the
whole of a cell back into the heap at once — cooling gives the bytes back after a burst (ADR 0023)
and the next ranking takes them all again. The cost of a cell is therefore not what it holds on
disk but what it costs to look at, and that grows with every retake.

On an iPhone it arrived as a refusal. Motion was unavailable, so guidance never advanced and five
bursts landed on one cell: 25 candidates of 1280×960×4 — **123 MB against a 128 MB ceiling**, since
Safari does not report `navigator.deviceMemory` and the store takes its stated fallback (ADR 0023).
Cooling had done its job. The set simply came back.

The motion bug is fixed and the pile-up with it, so this is no longer the failure in front of us.
It is still reachable on any device, by retaking one cell enough times, and the Pixel's own numbers
say a retake is a thing people will do.

## Decision

**A cell keeps the best `kMostCandidatesPerCell` candidates, and forgets the frames of the rest.**
The trim runs after ranking, on both paths that rank — a completed burst and an offered frame —
because the cell is best-first by then and the surplus is the tail.

**Eight, and the arithmetic is the argument.** The peak is what is kept plus the burst being ranked
into it. Eight kept and a default burst of five is thirteen frames: 64 MB on the phone above, half
its ceiling, leaving room for the preview frame the page keeps resident and the one being scored.
It also leaves a retake genuinely competing — five new frames ranked against eight kept can win
some places without taking all of them, which a cap of five could not promise.

**Forgotten, not cooled.** A candidate nothing will rank again is not evidence. Spilling it would
bound the cell and not the disk, and the disk is what a sphere of retakes fills.

**Never a frame the caller offered.** `OfferFrame` takes a handle the caller still owns — a file
import, a manual shutter — and `Cool` already declines to change the residency of one, because a
borrowed handle changing underneath its owner is a surprise they cannot expect. Forgetting one is
that mistake with nothing left to recover: the caller still holds the handle and the bytes are
gone. So a cell can exceed the cap, by being offered more frames than it. That is the caller's
arithmetic to do, and a manager that deleted their frames to keep its own number tidy would be
answering a question nobody asked it.

## Consequences

A cell's ranking cost is bounded, and so is its share of the spill tier. A capture can retake one
cell indefinitely without the memory growing.

Candidates are destroyed rather than hidden. Whatever a review strip eventually shows for a cell,
it will show at most this many — and the ones it does show are the ones the selection engine put
first, which is the order `Candidates` already promises.

**A trim ends only what the ranking named.** `IFrameQualityEngine::Rank` says "ranked best-first"
and does not say "all of them", and `Reorder` has always tolerated that gap by appending whatever
the ranking left out rather than dropping it. That puts an unranked candidate at exactly the end of
the cell a trim reaches for, where position alone cannot tell "judged and placed last" from "never
compared" — so `Reorder` now reports how long its ranked prefix is, and only inside that prefix is
a candidate this manager's to end. A frame the engine declined to look at is not one it called
worst, and ending it silently is the one outcome here that cannot be undone.

**The number is stated rather than derived, and that is the weak part.** ADR 0023 rejected a
stated heap ceiling in favour of probing `navigator.deviceMemory`, and the same argument applies
here: the store knows its ceiling, a frame's size is on its handle, and a cap could be computed
from both instead of chosen. It is not, because the failure this closes is unbounded growth and a
bound of any reasonable size closes it; deriving one adds a second thing that can be subtly wrong
on a device nobody here has. **What would change the decision:** a device whose ceiling makes
thirteen frames too many. The arithmetic to redo it is in the constant's comment, next to the
number.

**A trim that cannot end a frame keeps its candidate.** `Forget` refuses in two ways that leave
the store's entry in place — a pin it has promised a span to, a sink that would not drop the copy —
and it says so precisely because the budget goes on accounting for those bytes. Dropping the
candidate anyway would throw away the last handle to a frame the store is still charging for: an
orphan nothing can name, free, checkpoint or resume, which is a worse failure than the unbounded
growth this ADR exists to stop, because it cannot be recovered from at all. Keeping it costs a
place under the cap for memory that is being spent either way, and the next trim tries again.
`NotFound` is the exception, and not a refusal: the store is not holding the frame, so there is
nothing to keep a handle to and the candidate would be a row pointing at nothing.

**The cap counts the frames a cell keeps, not the frames this manager owns.** Ranking reads every
candidate's pixels regardless of who allocated them, so an offered frame takes a place under the
cap like any other: never forgotten, because it is not this manager's to end, but not free either.
Counting only what a trim *could* forget would let a cell sit at the cap plus every offered frame
in it, which is more heap during a rank than the number was chosen to allow.

Two things can therefore still push a cell past the cap, and both are named rather than left to be
found. A caller that offers more frames than the cap fills it with candidates nothing here may end
— its own arithmetic, with handles it allocated and holds. And an engine whose ranking omits
candidates leaves those outside anything a trim may reach; they sort after everything the engine
placed, so they cost a ranked candidate nothing and are kept in addition to the cap rather than
within it. Both are the state before this ADR rather than a new one, with `Allocate`'s refusal
underneath them exactly as before.

## Alternatives rejected

**Rank incrementally, so the whole set never comes back at once.** This attacks the real cost
rather than the symptom, and it is the wrong shape. `IFrameQualityEngine::Rank` takes the whole set
because the comparison is between candidates — exposure agreement is measured against the rest of
the burst, which is what makes a selection rather than a threshold. Ranking a new frame against a
summary of the old ones would weaken exactly the judgement the engine exists to make, and it would
put a memory concern inside V6, whose business is comparing frames rather than knowing how many of
them fit.

**Cap the burst instead, so a cell cannot grow.** That is a cap on retakes, which is the feature.

**Keep the candidates and drop only their frames.** A candidate whose pixels are gone is a row in
a strip that cannot be shown and a selection that cannot be honoured. The coverage map would go on
counting a cell as captured by evidence nothing can produce, which is ADR 0029's "a map that says
done, pointing at frames nothing can ever build from".
