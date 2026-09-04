# ADR 0026 — A cell's candidates come back ranked, and the review client reads that order

**Status:** accepted

## Context
`CaptureSessionManager` asks `IFrameQualityEngine::Rank` on every committed burst, checks that it
succeeded, and throws the answer away. `Candidates(node)` then hands back the order the shutter
fired in — which is not an opinion about anything.

That was invisible while nothing displayed a burst. Review Client v1 displays one, and a strip in
capture order is either useless or an invitation for the client to sort it, which would put "what
best means" in a client. V6 is the most-tuned axis in the system and `SelectionPolicy` exists so it
can be tuned in one place; a client re-deriving it from the scores would be a second place, and one
that could not see the normalisation `Rank` does across the set.

## Decision
**`Candidates(node)` returns the cell ranked best-first**, and the contract says so. The manager
keeps the order `Rank` already gave it rather than computing it twice or discarding it.

`OfferFrame` ranks too. An imported frame is evidence like any other and may be the best of the
set; appending it unranked would hold a better frame behind worse ones for the rest of the
session. When nobody can rank it the offer is rolled back and the failure reported, symmetrically
with the burst path — a cell holding a candidate the selection engine has already failed on is
worse than a rejected offer, because the failure is invisible and the strip is in an order nothing
chose.

**A ranking that does not name every candidate keeps the strays**, at the end. An engine returning
a short list is misbehaving; losing a captured frame to it would be a worse answer to that than an
oddly ordered strip.

## Consequences
- The automatic pick is `Candidates(node)[0]`, so the review client can name what a manual
  override is overriding without deciding anything.
- Ranking is O(n²) in a cell — a burst plus offers, single digits. A map keyed by candidate id
  costs more to build than this costs to walk at that size, and would be rebuilt on every change.
- `OfferFrame` can now fail for a reason that has nothing to do with the frame offered. That is
  the same trade the burst path already makes and the alternative is worse.
- **Nothing persists the ranking.** It is recomputed whenever the cell changes and lives only in
  the manager's memory, so it goes with the session. That is fine while selection is also
  in-memory (below) and becomes a question when either is persisted.

## Rejected alternative
**Expose the ranking separately** — leave `Candidates` in capture order and add
`Ranking(node)` returning ids. It keeps two facts apart that a caller almost always wants
together, and every caller would immediately join them, in each caller's own way. The order a set
is handed over in is a perfectly good place to put an opinion about the set, and the contract can
say which opinion it is.
