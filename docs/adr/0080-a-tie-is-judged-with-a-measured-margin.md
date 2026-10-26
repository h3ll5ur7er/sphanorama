# 0080 — A tie is judged with a measured margin, and an echo is not evidence

**Status:** accepted; amends [0073](0073-a-tie-names-its-whole-piece.md), whose unseen ties it
narrows

## Context

`ambiguous` names the pieces where some placement was decided by an arbitrary choice rather than
the evidence (ADR 0073). A tie was an average whose top two eigenvalues fell within
`AverageQuaternions`' threshold, a relative 1e-12, and ADR 0073 recorded two ways the solver splits
a tie in the evidence by far more than that, leaving `ambiguous` empty:

- **An echo.** A neighbour that, with the frame removed, is joined to no anchor is placed from that
  frame and nothing else, so its prediction for the frame is the frame's own place handed back.
  Averaged in, it outvotes evidence that cancels, and the frame stays wherever the walk first put
  it, which the anchors' index order decides. One unanchored leaf on the tied frame of a committed
  fixture emptied the list, and swapping two frames' labels moved the tied frame a half turn.
- **A residual.** A neighbour updated earlier in the same sweep tilts a frame's predictions, so a
  tie exact in the input reached the average a relative 1e-6 apart, a million times the threshold.

Two measures say a frame sits where an arbitrary choice put it, both with the solves converged: a
1e-12 nudge to one edge or anchor moves it more than a thousandth of a degree, or renumbering the
frames and shuffling the edges moves it more than a tenth. The nudge cannot see an echo, which makes
the coin's placement stable; the renumbering can, and also counts answers that depend on the order
without balancing on a tie. Over 20,000 contradictory graphs of three to seven frames, with
rotations drawn from the identity, half turns, a quarter and a third, 309 left out a frame the nudge
moves, and 1,544 one that either moves.

## Decision

- **A tie is a relative separation under 1e-3**, for a frame's own average and for a piece's gauge.
  `AverageQuaternions` reports the separation, and the solver judges it against its own threshold;
  `isUnique` keeps the eigensolver's. For two predictions at equal weight a thousandth is 0.057
  degrees short of a half turn, which no registration that worked produces.
- **An echo is not averaged into the frame it hangs from.** For each frame, the believed edges
  without it fall into parts, found by flooding from each neighbour, and an edge into a part with no
  anchor is not averaged in. A bridge to a leaf is one case; a leaf held by two edges, or an
  unanchored cycle hanging off the frame, has no bridge and is the same echo. The frames beyond
  still average the frame they hang from. No flood starts from an anchored neighbour, which is
  evidence by being one, so where every frame is anchored, as in a build, the search costs a pass
  over the edges; a flood per frame costs frames times edges, which at 200 frames and 800 edges
  is about five sweeps.

## Consequences

- Both cases the reviews constructed are named, and so are the leaf held by two edges and the
  hanging triangle, each with its labels swapped. Over the same 20,000 graphs, 261 leave out a frame
  the nudge moves, against 309, and 1,341 one that either measure moves, against 1,544. Searching
  for bridges alone did no better on the nudge, 263, and worse on both, 1,410, because the nudge
  cannot see the echoes it missed. Over 3,000 graphs with consistent edges jittered half a degree
  and priors three degrees out, nothing is named, as before.
- **Ties remain that no test of the final answer can see.** The solve can pass through a tie and
  then settle into a basin it is stable in. On the solver before this change, pushing every frame
  five degrees off the answer and relaxing again still left 167 of 289 nudge-moved graphs
  unexplained, since they came back. On three seeds of four, one to three of what is left have
  every edge within a degree, because what the coin decided was where a whole piece sits, between
  anchors that nearly cancel, and no edge can see a piece turned bodily. So `maxEdgeErrorDeg` is not
  a stand-in for the list, and the list is not complete. The header says so.
- **Leaving an echo out moves no fixed point; it changes the path to one.** A part with no anchor
  can turn bodily, so where the solve stops its edges pull on the frame no more than the frame pulls
  back: measured, an unanchored triangle thirty degrees out of true, hanging by one edge, moved its
  frame by 1.5e-5 degrees. The path is where a tie is broken, which is the point. At an
  `anchorWeight` of zero, where nothing turns a piece onto its anchors, the path also decides the
  gauge: on the 3,000 consistent graphs the answer turned bodily by up to 1.35 degrees, its shape
  within 4e-4. In `Refine` every frame with a sensor reading is anchored, and a build pairs no frame
  without one, so a build holds no echo.
- **More is named, some of it decided by the evidence.** Over the 20,000 graphs frames were named
  in 3,180 against 3,041, and the graphs named where no nudge moves anything rose too, 476 to 503.
  A separation is a ratio, so two opposing edges whose weights are within a thousandth of each
  other, 1,001 inliers against 1,000, are named, though the heavier one decides and no order or
  nudge changes it.
- ADR 0073's figure of 250 unnamed graphs came from the review's own generator; the 309 and 289
  here are this one's, on the same kind of input.
- `GlobalSolution::ambiguousFrames` says what it can and cannot see in the contract comment, which
  was regenerated into the TypeScript mirror.

## Rejected alternatives

Both were measured on the nudge alone, by a harness that draws its 20,000 graphs the same way but
not the same graphs, since the renumbering takes from the same generator; there the solver before
this change left 289 out.

**Re-solve in another order.** Solve again with the walk seeded and the sweep run backwards, and
name the frames whose place differs beyond the convergence noise, the gauge removed per piece. It
sees ties decided partway through a solve, which nothing at the answer can: 120 graphs left a frame
out. It also names 2,190 graphs where no nudge moves anything: answers that depend on the order,
which the renumbering measure above counts as misses, so those names are earned rather than
spurious. It doubles every solve, and `Refine` solves once per focal-length trial on the phone,
where how long a sphere takes is not yet measured. This is the next step if the list is to be made
more complete, and it is a cost to decide on rather than one to slip in.

**Push the answer and relax it again.** Turn every frame a few degrees off the converged answer,
relax, and name what does not come back. It finds an unstable balance directly, without a threshold
on the eigenvalues, but it doubles the sweeps as well, and it misses more: 167 graphs, at every push
from a thousandth of a radian to twenty degrees. A tie the solve passed through and settled away
from comes back as reliably as an answer the evidence decided.
