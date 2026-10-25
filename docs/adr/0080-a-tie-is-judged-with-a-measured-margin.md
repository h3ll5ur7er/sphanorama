# 0080 — A tie is judged with a measured margin, and an echo is not evidence

**Status:** accepted; amends [0073](0073-a-tie-names-its-whole-piece.md), whose unseen ties it
narrows

## Context

`ambiguous` names the pieces where some placement was decided by an arbitrary choice rather than
the evidence (ADR 0073). A tie was an average whose top two eigenvalues fell within
`AverageQuaternions`' threshold, a relative 1e-12, and ADR 0073 recorded two ways the solver splits
a tie in the evidence by far more than that, leaving `ambiguous` empty:

- **An echo.** A neighbour whose side of its edge holds no anchor is placed from the frame it hangs
  from and nothing else, so its prediction for that frame is the frame's own place handed back.
  Averaged in, it outvotes evidence that cancels, and the frame stays wherever the walk first put
  it, which the anchors' index order decides. One unanchored leaf on the tied frame of a committed
  fixture emptied the list, and swapping two frames' labels moved the tied frame a half turn.
- **A residual.** A neighbour updated earlier in the same sweep tilts a frame's predictions, so a
  tie exact in the input reached the average a relative 1e-6 apart, a million times the threshold.

The measure is a 1e-12 nudge to one edge or anchor: a frame it moves by more than a thousandth of a
degree, with both solves converged, sits where an arbitrary choice put it. Over 20,000 contradictory
graphs of three to seven frames, with rotations drawn from the identity, half turns, a quarter and
a third, 289 left such a frame out of `ambiguous`.

## Decision

- **A tie is a relative separation under 1e-3**, for a frame's own average and for a piece's gauge.
  `AverageQuaternions` reports the separation, and the solver judges it against its own threshold;
  `isUnique` keeps the eigensolver's. For two predictions at equal weight a thousandth is 0.057
  degrees short of a half turn, which no registration that worked produces.
- **An echo is not averaged into the frame it hangs from.** Edges whose far side holds no anchor are
  bridges, found once by a depth-first search over the believed edges with the anchors on each side
  counted. The frames beyond still average the frame they hang from.

## Consequences

- Both cases the reviews constructed are named, and over the same 20,000 graphs 243 leave a frame
  out, against 289; across four seeds, 243 to 270 against 285 to 315. Over 3,000 graphs with
  consistent edges jittered half a degree and priors three degrees out, nothing is named, as before.
- **Ties remain that no test of the final answer can see.** The solve can pass through a tie and
  then settle into a basin it is stable in. On the solver before this change, pushing every frame
  five degrees off the answer and relaxing again still left 167 of the 289 graphs unexplained, since
  they came back. A few of what is left have every edge within a degree,
  because what the coin decided was where a whole piece sits, between anchors that nearly cancel,
  and no edge can see a piece turned bodily. So `maxEdgeErrorDeg` is not a stand-in for the list,
  and the list is not complete. The header says so.
- **An echo that agrees changes nothing; one that disagrees no longer bends its frame.** An exact
  echo returns the frame's own place, so leaving it out leaves the fixed point where it was. A
  piece hanging by one edge whose own cycles disagree used to pull the frame it hangs from by its
  disagreement, and does not now. In `Refine` every frame with a sensor reading is anchored, so an
  echo there needs a frame without one.
- Separations between 1e-12 and 1e-3 are now named where they were not. Over the 20,000 graphs,
  frames were named in 3,025 against 2,920.
- `GlobalSolution::ambiguousFrames` says what it can and cannot see in the contract comment, which
  was regenerated into the TypeScript mirror.

## Rejected alternatives

**Re-solve in another order.** Solve again with the walk seeded and the sweep run backwards, and
name the frames whose place differs beyond the convergence noise, the gauge removed per piece. It
sees ties decided partway through a solve, which nothing at the answer can: 120 of the 20,000 graphs
left a frame out. It doubles every solve, and `Refine` solves once per focal-length trial on the
phone, where how long a sphere takes is not yet measured. It also names 2,190 graphs where no nudge
moves anything: answers that depend on the order without balancing on a tie, which the header has
called a hazard of contradictory input rather than an arbitrary placement. This is the next step if
the list is to be made more complete, and it is a cost to decide on rather than one to slip in.

**Push the answer and relax it again.** Turn every frame a few degrees off the converged answer,
relax, and name what does not come back. It finds an unstable balance directly, without a threshold
on the eigenvalues, but it doubles the sweeps as well, and it misses more: 167 graphs, at every push
from a thousandth of a radian to twenty degrees. A tie the solve passed through and settled away
from comes back as reliably as an answer the evidence decided.
