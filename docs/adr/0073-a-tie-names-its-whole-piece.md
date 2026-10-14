# 0073 — A tie names its whole piece

**Status:** accepted

## Context

`AverageRotations` reports in `ambiguous` the frames whose average had no single maximiser on some
sweep: the evidence for the frame cancels — two predictions a half turn apart, or three a third of
a turn apart about one axis — so where it lands is the eigensolver's scan order rather than the
evidence. It passes through to `GlobalSolution::ambiguousFrames`. The header promised it answers
whether the answer rests on an arbitrary choice.

A review of PR #94 found a frame it does not name. In a five-frame graph — anchor on frame 0; edges
4→1 a half turn about Z at weight 2, 4→0 a half turn about the diagonal of X and Y at weight 3, 1→0
the identity at weight 2, and 2→3 a half turn about Z joining two frames with no anchor — frame 1
is told 0 by frame 0 and a half turn by frame 4, and ties. Frame 4 does not: its two predictions
have a single maximiser. But one of them is made from wherever the coin put frame 1. Measured, a
ten-trillionth of a radian about X or Y on any of the first three edges moves frame 1 70.5 degrees
and frame 4 38.9, and `ambiguous` names frame 1 alone. Frame 0, the anchor, does not move: with one
anchor, the solver turns the piece onto it every sweep.

A piece is the frames a chain of believed edges joins. No edge and no gauge step reaches from one
piece into another, so a coin flipped in one moves nothing in the next — beyond when the solve
stops: the sweep count is shared, so a tie that keeps one piece moving gives the others more sweeps.
A review measured that at 1.3e-5 degrees, about the stopping tolerance.

## Decision

**When any frame of a piece ties, on any sweep, every frame of the piece is named.** The same holds
already for a piece whose anchors disagree by a half turn about where it sits (ADR 0064), so one
rule now covers both: `ambiguous` is the frames of the pieces where an arbitrary choice was seen.

It is decided after the sweeps, from the per-frame flags and the pieces the solver already holds.
The list is still built from flags, each frame once and ascending.

## Consequences

- **No frame that depends on a tie the solver sees goes unnamed.** A frame's tie means a
  registration has gone badly wrong; an anchor tie means the priors contradict each other. Either
  way the piece is the place to look.
- **Not every tie is seen.** A tie is an average whose top two eigenvalues fall within
  `AverageQuaternions`' threshold, and the arithmetic can split one in the evidence by far more than
  that. A neighbour joined to nothing else echoes a frame's own place back, so it never ties, and
  the walk's seed order — frame labels — puts it at 0 or at a half turn. A neighbour updated earlier
  in the sweep can tilt an exact tie by a millionth, and a ten-trillionth of a radian then moves the
  frame 90 degrees. Both leave `ambiguous` empty, and both read `maxEdgeErrorDeg` at 180. A review's
  fuzz of 20,000 contradictory graphs found 250 such. Detecting them is its own change.
- **It names frames that never move, possibly all of a capture.** The anchor of a one-anchor piece
  is pinned every sweep, and so is everything on the far side of it from the tie: a tail hanging off
  a lone anchor is named, and a nudge moves it only at the rounding floor. One contradictory pair
  can name the whole sphere. That is the price of not computing which frames depend on the choice,
  paid only on input already contradictory.
- **Which frame tied is no longer readable from the list.** The edge errors still tell the two kinds
  apart where the anchors are weighed lightly, as `Refine` weighs them: a frame's tie leaves an edge
  far out, while an anchor tie leaves the edges in agreement, since the gauge is what the priors
  decide. Heavy anchors bend the edges in both.
- `GlobalSolution::ambiguousFrames` means the same, and its contract comment says so. Nothing acts
  on it yet; `FeatureRegistrationEngine::Refine` passes it through.
- The tests that pinned index zero, the last index and ascending order did so with one tied frame
  per fixture. Whole pieces include frame zero trivially, so they are now two-piece fixtures where
  only one piece ties, and the first piece is walked 0, 3, 1 so a list built in walk order is
  caught.

## Rejected alternatives

**Two lists**: keep `ambiguous` as the frames that tied and add a field for their pieces. It keeps
"where is the contradiction" for a frame's tie, at the cost of a second contract field whose only
reader would be the same caller — and the edge errors already say it for that kind, while an anchor
tie has no single frame to name.

**Measure dependence**: re-solve with the edges nudged and name the frames that move more than a
threshold. The most precise answer, but it doubles the solve whenever a tie occurs, and the
threshold would itself need measuring — a nudge that breaks a tie moves frames by any amount up to a
half turn.

**Cut at the anchors.** With one anchor and its weight above zero, the anchor is put back on its
prior every sweep, so frames beyond it from the tie cannot move; the block structure of the graph
would find them without re-solving. Exact for that case and nothing else — two anchors, or an
anchor weight of zero, and it says nothing — for a structure computed on every contradictory solve.

**Propagate along the edges a frame read from.** It names whatever the sweeps happened to read
before they settled, so the answer would depend on how many ran; given enough, it is the whole
piece, pinned anchor included — the decision above, reached by a route that can stop short.

**Near-ties.** A nudge of a billionth of a radian breaks the tie and empties the list, while the
frames still move as far. That is the averaging's threshold (`kEigenvalueGap`), which claims ties a
few ulps wide and is not a conditioning test; a conditioning test is a different question from this
one, and the same one the unseen ties above raise.
