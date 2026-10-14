# 0073 — A tie names its whole piece

**Status:** accepted

## Context

`AverageRotations` reports in `ambiguous` the frames whose average had no single maximiser on some
sweep — two pieces of evidence for the frame a half turn apart, so where it lands is the
eigensolver's scan order rather than the evidence. It passes through to
`GlobalSolution::ambiguousFrames`. The header promised it answers whether the answer rests on an
arbitrary choice.

A review of PR #94 found a frame it does not name. In a five-frame graph — anchor on frame 0; edges
4→1 a half turn about Z at weight 2, 4→0 a half turn about the diagonal of X and Y at weight 3,
1→0 the identity at weight 2 — frame 1 is told 0 by frame 0 and a half turn by frame 4, and ties.
Frame 4 does not: its two predictions have a single maximiser. But one of them is made from wherever
the coin put frame 1. Measured, a ten-trillionth of a radian on any edge moves frame 1 70.5 degrees
and frame 4 38.9, and `ambiguous` names frame 1 alone. Frame 0, the anchor, does not move: with one
anchor, the solver turns the piece onto it every sweep.

A tie reaches no further than its piece. A piece is the frames a chain of believed edges joins, and
nothing relates one piece to another, so a coin flipped in one cannot move a frame in the next.

## Decision

**When any frame of a piece ties, on any sweep, every frame of the piece is named.** The same holds
already for a piece whose anchors disagree by a half turn about where it sits (ADR 0064), so one
rule now covers both: `ambiguous` is the frames of the pieces where an arbitrary choice was made.

It is decided after the sweeps, from the per-frame flags and the pieces the solver already holds.
The list is still built from flags, each frame once and ascending.

## Consequences

- **No frame that depends on a tie goes unnamed**, which is the property a caller deciding what to
  trust needs. A tie means a registration has gone badly wrong, and that piece is the place to look.
- **It names frames that did not move.** Frame 0 above is pinned by its own anchor and is named with
  the rest. That is the price of not computing which frames depend on the choice, and it is paid
  only on input that is already contradictory.
- **Which frame tied is no longer readable from the list.** `maxEdgeErrorDeg` still says the piece's
  edges disagree, and the edges themselves say where.
- `GlobalSolution::ambiguousFrames` means the same, and its contract comment says so. Nothing acts
  on it yet; `FeatureRegistrationEngine::Refine` passes it through.
- The tests that pinned index zero, the last index and ascending order did so with one tied frame
  per fixture. Whole pieces include frame zero trivially, so they are now two-piece fixtures where
  only one piece ties, and the first piece is walked 0, 3, 1 so a list built in walk order is
  caught.

## Rejected alternatives

**Two lists**: keep `ambiguous` as the frames that tied and add a field for their pieces. It keeps
"where is the contradiction", at the cost of a second contract field whose only reader would be the
same caller, and the edge errors already locate the contradiction.

**Measure dependence**: re-solve with the edges nudged and name the frames that move more than a
threshold. The most precise answer, but it doubles the solve whenever a tie occurs, and the
threshold would itself need measuring — a nudge that breaks a tie moves frames by any amount up to a
half turn.

**Propagate along the edges a frame read from.** It names whatever the sweeps happened to read
before they settled, so the answer would depend on how many ran; given enough, it is the whole
piece, pinned anchor included — the decision above, reached by a route that can stop short.

**Near-ties.** A nudge of a billionth of a radian breaks the tie and empties the list, while the
frames still move as far. That is the averaging's threshold (`kEigenvalueGap`), which claims only
exact ties and says so; a conditioning test is a different question from this one and is not decided
here.
