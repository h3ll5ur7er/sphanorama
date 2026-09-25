# 0064 — The rotation solver chooses each piece's gauge from its anchors every sweep

**Status:** accepted. Withdraws the sweep table and the `590` that ADR 0062 published.

## Context

`Refine` will hand `AverageRotations` (ADR 0062) the pairs `EstimatePairwise` registered, weighed
by their inlier counts, and the phone's orientation for each frame as its anchor. Choosing the
anchor weight for it was a measurement, run on the twelve-frame photograph ring the accuracy table
is measured on: every consecutive pair and the closing one registered, anchors each three degrees
out about an axis of their own, `(sin i, cos i, sin 2i)` for frame `i` — the construction the
solved-ring accuracy test uses. Scored gauge-free, the answer's shape is
best with the anchors weighed very lightly — the edges are out by hundredths of a degree and the
anchors by degrees:

| Edges weighed by | Anchor weight | ORB | AKAZE | SIFT |
| --- | --- | --- | --- | --- |
| (the chain, closing edge discarded) | — | 0.1009 | 0.0612 | 0.0239 |
| 1 each | 1e-6 to 1e-3 | 0.0555 to 0.0568 | 0.0345 to 0.0366 | 0.0238 to 0.0255 |
| 1 each | 0.01 | 0.0728 | 0.0603 | 0.0407 |
| 1 each | 1 | 1.4747 | 1.4909 | 1.4995 |
| inlier count | 1e-6 to 0.1 | 0.0536 to 0.0558 | 0.0348 to 0.0350 | 0.0256 to 0.0266 |
| inlier count | 1 | 0.1292 | 0.0772 | 0.0322 |

(median degrees from the truth after the gauge is removed; inliers ran 15 to 215 per pair. A first
version of this table was measured with anchor axes drawn from a random generator rather than the
construction above, and did not reproduce from anything in the tree; a reviewer caught it.)

But the same runs said the solver could not deliver that answer, in two ways:

- **It ran out of sweeps.** Weighed per edge at 1e-4 and 1e-3, every detector used the whole
  budget of 1,000 and returned `converged == false`. So did inlier-weighted edges from 0.01 to 0.1.
- **When it said it had converged, the gauge was wherever it started.** Inlier-weighted at 1e-4, it
  settled in 113, 72 and 48 sweeps with the gauge 0.546, 0.476 and 0.426 degrees from the truth,
  where the gauge the twelve anchors agree on best is 0.261 degrees out. Each sweep moves the gauge
  by about as much as the anchors weigh against the edges, so a light anchor makes it crawl, and
  a move under the stopping tolerance on every sweep is `converged` whether or not it has arrived.

The gauge is the one thing the anchors are there to decide — the edges are relative, and cannot say
which way is up. A solver whose gauge falls where its start put it when the caller weighs the
anchors lightly is not doing the job ADR 0062 gave it, and the light weight is exactly what the
table above says a caller should choose.

## Decision

**After each sweep, every piece of the reconstruction is turned bodily onto the gauge its anchors
agree on best.** A piece is a set of frames that a chain of believed edges joins; each has at least
one anchor, since placement starts only from one. The turn is the chordal average of
`anchor * conjugate(solved)` over the piece's anchored frames — the rotation that, applied to every
frame, brings the piece nearest all of its anchors at once — and every frame of the piece is turned
by it, anchored or not.

- **It changes no edge's agreement**, since every edge joins two frames of one piece and both turn
  together. So it maximises the objective the per-frame sweep is already climbing, over the one
  direction that sweep is slowest in: the fixed points are the same and the slow direction is gone.
- **Per piece, never across pieces.** Nothing relates one piece's orientation to another's.
- **Skipped at an anchor weight of zero**, which keeps its documented meaning: the anchors place the
  frames and are not consulted again.
- **The turn counts as a move** for the stopping rule. When the anchors barely determine the gauge,
  the best one moves much further than the frames do: three frames whose anchors turn them a third
  of a turn apart, less a tenth of a degree, settle their shape while their gauge is still 0.0116
  degrees from the fixed point.
- **A piece whose anchors disagree by a half turn names every frame in it as `ambiguous`**, because
  which anchor the whole piece sides with is then the eigensolver's scan order. Before this, that
  was decided silently: the first frame visited moved onto the other's anchor and nothing was named.

Measured on the same ring, the same runs: both weightings at every anchor weight from 1e-6 to 10
converged, in 6 to 114 sweeps, with the gauge within 0.0003 degrees of where the anchors agree (0.261 from the truth), and every
shape figure the old solver reached a converged answer for within 0.0001 degrees of it.

## Consequences

**The sweep budget is set by the frame count now, not by the anchor weight.** Rings with exact edges
and anchors three degrees out, sweeps to settle (the table beside `kMaxSweeps` carries it in full):

| frames | 1e-6 | 1e-4 | 0.01 | 1 |
| --- | --- | --- | --- | --- |
| 12 | 48 | 48 | 46 | 15 |
| 90 | 859 | 842 | 327 | 15 |
| 200 | out of budget | out of budget | 403 | 16 |

What is left to settle is the shape, and the slowest bend of a ring relaxes at a rate that falls as
the square of its length. 1,000 still covers every ring up to ninety frames at every weight.
A two-hundred-frame ring weighed lightly is the fixture that runs out of budget now, since nothing
at twelve frames does.

**Figures withdrawn** (the trigger in `docs/00-principles.md`):

- ADR 0062's sweep table, and its `590` for the twelve-frame ring at 0.01 as "the executable copy"
  of it. That cell is 46 now, asserted in the same place. The table ran the other way before —
  27,676 sweeps at two frames and 1e-4 against 894 at ninety — because what was moving then was
  the gauge.
- The stopping tolerance's *demonstration*, not its bracket. `kSettledDeg` sits just above a regime
  that stops converging, shown by ninety frames at an anchor weight of ten never settling at 1e-6.
  They settle in 11 now, but sixty frames at ten and two hundred at one and at ten never do, within
  200,000 sweeps, so the bracket stands on those rows instead. A first version of this ADR withdrew
  the bracket itself, from the one row; a reviewer ran the others.
- The census of which tests move with that tolerance, in `rotation_averaging_test.cpp`, re-measured.

**Unchanged:** the closing-edge figures CLAUDE.md and the roadmap publish, 1.100000 against
0.0000278 degrees (and 0.0000024 at the fixed point), and the shape figures of every existing
fixture. They are the same fixed points, reached sooner.

**Turning every frame of a piece, anchored or not, is load-bearing.** Turning only the anchored
frames breaks the edges between them and the rest every sweep, and where the anchors are few the
fixed point moves: two frames, one anchored, joined by pairs claiming 30 and 38 degrees on 300 and
100 inliers, settle at 30.05 rather than 32. `Refine.APairCountsForItsInliers` is what catches it.
A first version of this paragraph said the sabotage survived every fixture, which was true of the
solver's own suite and not of the engine's.

## Rejected alternatives

**Weigh the anchors heavily enough that the sweep converges on its own.** At 0.01 per unit edge it
does, in about 590 sweeps on the photograph ring, and the shape pays for it: SIFT's median goes from
0.024 to 0.041 degrees, worse than the chain it is meant to improve on.

**Solve with the anchors at zero, then turn the answer onto them once at the end.** One turn is the
same step as this, taken once. It gives up everything a positive anchor weight is for — an edge
that is wrong by more than the anchors, pulled back toward them — and makes the weight a caller
passes mean nothing between zero and the point where it starts bending the shape.

**Raise the budget.** The synthetic twelve-frame ring wanted about 12,500 sweeps at a
ten-thousandth, and at a millionth the budget was never the limit: the solve stopped after 48 sweeps
and said it had converged, with the gauge 0.35 degrees from where the anchors agree. More sweeps do not fix a
stopping rule that cannot see what is still moving.
