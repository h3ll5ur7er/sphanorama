# 0064 — The rotation solver chooses each piece's gauge from its anchors every sweep

**Status:** accepted. Withdraws the sweep table and the `590` that ADR 0062 published.

## Context

`Refine` will hand `AverageRotations` (ADR 0062) the pairs `EstimatePairwise` registered, weighed
by their inlier counts, and the phone's orientation for each frame as its anchor. Choosing the
anchor weight for it was a measurement, run on the twelve-frame photograph ring the accuracy table
is measured on: every consecutive pair and the closing one registered, anchors each three degrees
out about an axis of their own. Scored gauge-free, the answer's shape is best with the anchors
weighed very lightly — the edges are out by hundredths of a degree and the anchors by degrees:

| Edges weighed by | Anchor weight | ORB | AKAZE | SIFT |
| --- | --- | --- | --- | --- |
| (the chain, closing edge discarded) | — | 0.1009 | 0.0612 | 0.0239 |
| 1 each | 0.01 | 0.0589 | 0.0429 | 0.0428 |
| 1 each | 1 | 1.3816 | 1.3615 | 1.3573 |
| inlier count | 1e-4 to 0.1 | 0.0503 to 0.0558 | 0.0313 to 0.0348 | 0.0254 to 0.0266 |
| inlier count | 1 | 0.0916 | 0.0602 | 0.0332 |

(median degrees from the truth after the gauge is removed; inliers ran 15 to 215 per pair.)

But the same runs said the solver could not deliver that answer, in two ways:

- **It ran out of sweeps.** Weighed per edge at 1e-4 and 1e-3, every detector used the whole
  budget of 1,000 and returned `converged == false`. So did inlier-weighted edges from 0.01 to 0.1.
- **When it said it had converged, the gauge was wherever it started.** Inlier-weighted at 1e-4, it
  settled in 112, 72 and 48 sweeps with the gauge 1.385, 1.117 and 0.983 degrees from the truth,
  where the gauge the twelve anchors agree on best is 0.715 degrees out. Each sweep moves the gauge
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

Measured on the same ring, the same runs: both weightings at every anchor weight from 1e-4 to 10
converged, in 6 to 112 sweeps, with the gauge 0.715 degrees out, and every shape figure within
0.0001 degrees of the table above.

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
- The stopping tolerance's bracket. `kSettledDeg` was argued to sit just above a regime that stops
  converging, from ninety frames at an anchor weight of ten never settling at 1e-6. They settle in
  11 now, so the tolerance is chosen for cost above the `AngleBetween` floor rather than bracketed.
- The census of which tests move with that tolerance, in `rotation_averaging_test.cpp`, re-measured.

**Unchanged:** the closing-edge figures CLAUDE.md and the roadmap publish, 1.100000 against
0.0000278 degrees (and 0.0000024 at the fixed point), and the shape figures of every existing
fixture. They are the same fixed points, reached sooner.

**One sabotage survives, and why is worth a sentence.** Turning only a piece's anchored frames
converges to the same answer in the same number of sweeps on every fixture here, because the next
sweep drags an unanchored frame back onto its edges before anything reads it. Turning every frame is
kept because it is the shorter statement and the one the "changes no edge's agreement" argument is
made about.

## Rejected alternatives

**Weigh the anchors heavily enough that the sweep converges on its own.** At 0.01 per unit edge it
does, in about 520 sweeps on the photograph ring, and the shape pays for it: SIFT's median goes from
0.024 to 0.043 degrees, worse than the chain it is meant to improve on.

**Solve with the anchors at zero, then turn the answer onto them once at the end.** One turn is the
same step as this, taken once. It gives up everything a positive anchor weight is for — an edge
that is wrong by more than the anchors, pulled back toward them — and makes the weight a caller
passes mean nothing between zero and the point where it starts bending the shape.

**Raise the budget.** The twelve-frame ring wanted about 12,500 sweeps at a ten-thousandth, and at
a millionth the budget was never the limit: the solve stopped after 48 sweeps and said it had
converged, with the gauge 0.35 degrees from where the anchors agree. More sweeps do not fix a
stopping rule that cannot see what is still moving.
