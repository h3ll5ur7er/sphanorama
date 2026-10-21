# 0076 — The planner checks its own plan

**Status:** accepted; amends [0075](0075-a-burst-frame-is-taken-only-on-its-cell.md); amended by
[0077](0077-a-capture-keeps-the-plan-it-was-begun-with.md), which makes a resumed capture keep its
plan

## Context

ADR 0075 spaced each ring where its band is widest, and recorded what that still left open. Rings
were laid as though the single frame at a pole reached half its vertical field in every direction,
and across its short edge it reaches less, so some lenses near the ones tried left holes at 73 to 75
degrees. At zero overlap, 62 of 276 lenses left holes with every frame on its cell's centre. And
every claim of completeness rested on chosen lenses, each of which passed while a lens a fraction of
a degree away did not.

A sweep of the real planner made the third point general. With pole frames taken at any heading,
since guidance asks for no roll within five degrees of a pole (ADR 0072), and every frame anywhere
in a 4 degree cone at 30% overlap, 31 of 141 long edges from 20 to 90 degrees failed on 4:3, 40 on
3:2 and 60 on 16:9. Most were narrow lenses, where a cone of 4 degrees on each side of a shared edge
eats an overlap of 30%. The rest were the caps.

Rules for each failure were tried next: steps capped at the field less two and a half cones, and
rings added until a pole frame's short half-edge reached halfway to the next ring. They cleared
that sweep. Asked again with an offset, denser sampling, they left directions open. Three cones
instead of two and a half failed lenses that two and a half had passed, because ring and cell
counts round, so a margin tuned until a sweep is clean is complete by rounding again, one level up.

## Decision

**`RingsCoveragePlannerEngine::Plan` checks the plan it lays, and lays it tighter until the check
passes.**
- **The check.** For each of 4000 directions spread evenly over the sphere, and 720 on the
  horizon, the question is whether some cell's frame covers it whether that frame is aimed at the
  cell's centre or at any of 16 points around its cone's edge. For one direction this is the worst
  case exactly, since each cell's frame is taken once. A plan without its caps answers only for
  the directions no higher than its highest ring, and the horizon is asked outright because a band
  left with only its horizon ring would otherwise be asked about nothing: no point of the spread
  lands on it.
- **The frame it asks about is a degree smaller than the lens on every side.** Asked of the lens
  itself, plans complete at the 4000 directions left two in 5000 open at another sampling. The
  degree is what a real frame spends covering the directions and aims between the ones asked.
- **When the check fails,** both steps shrink by 5% and the rings are laid again. A plan of more
  than 2000 cells, over an hour at a two-second dwell, ends the search, so a lens or cone that
  never closes is refused in a bounded time rather than hung on.
- **A cone too wide for its lens is refused**: one at least half the narrower field, less the
  degree. A frame aimed at its edge would not see its own cell's centre, and no spacing closes a
  sphere of those.

The steps start where ADR 0075 left them: the overlap's, with each ring spaced where its band is
widest. The rules tried above are not kept: over 102 phone lenses the check alone lays 5207 cells,
and the check with the rules 5217.

Pole frames are asked at any heading, every 15 degrees. A first version of this ADR left them out,
because no lens its tests swept needed them; a review found three that do, held sideways at zero
overlap, where a pole frame turned to its short edge leaves directions at 66 degrees open that
sampling at 5000 directions passes between. A test asks those three at 20,000.

## Consequences

- The page's lens plans as before: 32 cells held upright and 38 held sideways, complete as first
  laid. Every 4:3 lens from 60 to 75 degrees plans the cells it did before this ADR.
- Lenses that left holes cost cells now. 16:9 held upright at 66 degrees goes from 39 cells to 64,
  3:2 at 43.5 degrees from 80 to 111, and 4:3 at 30 degrees from 169 to 254. Shrinking the two
  steps in turn rather than together was tried, and saved less than 1%.
- Planning takes about 5 milliseconds for the page's lens at `-O2`, and about as long for a 30
  degree lens. Each direction is asked of the cells nearest it first, nearest ring and then nearest
  in azimuth, and of a cell only if it lies within that frame's half-diagonal and cone. At the
  page's lens held upright, every cone it plans, up to 22.8 degrees and 1946 cells, plans within 20
  milliseconds, and every cone past that is refused within 20 as well. Over a grid of 4032 specs and
  4000 random ones (lenses from 3 to 180 degrees, cones up to the line, overlap up to 0.9, with and
  without the caps) the slowest took 106 milliseconds: a 172 degree lens at no overlap. In the
  browser's build, run under node, it took 305. Planning happens once, at `Begin` or `Resume`.
- The cell bound was a hundred thousand when this was first written, and a review found a cone a
  third of a degree short of the refusal planned sixty thousand cells in 15 seconds natively and
  over a minute in the browser. At 2000, cones that near the line are refused within 20
  milliseconds, with a detail that names the cone. The bound counts the cells laid: a first version
  compared it with a grid of full circles, half again as many, and refused every plan above about
  1300.
- The bound limits cells, not work. Asked in the order they were laid, a wide cone left nearly every
  cell near enough, and a direction walked half the plan before one covered it: a review found specs
  the bound allows taking over three seconds in the browser's build. Asking nearest first lays the
  same plans, identical over the 4032 specs above, and took the slowest the review found, 180 by
  101.25 degrees with a 48 degree cone, from 3158 milliseconds in that build to 54. That speed is
  measured, not tested: the laid order put back passes every test, about twenty times slower on the
  specs it matters for, and a wall-clock ceiling on a shared runner under a sanitizer would fail for
  reasons of its own. What is tested is that the order changes no answer, by cell counts at plans
  whose covering cell is not the nearest.
- Rings are counted from the horizon rather than from a pole, so the horizon is exactly 0 and the
  two sides are mirrors. Counted from a pole, a band whose limit was the horizon lost its horizon
  ring by an ulp and was laid tighter than asked. The change moves cells by ulps on about a tenth of
  specs, and a band whose limit lands exactly on a ring now keeps that ring more often: 170 by 144
  degrees at an overlap of 0.982, for one, keeps its ring at 18 degrees and lays 1746 cells where it
  laid 1633. Which way a ring exactly on the limit goes is still decided by rounding, the same way
  on both sides.
- Completeness is at the resolution checked, with the degree as its margin. The tests check plans at
  a denser and offset sampling: 5000 directions, 24 aims and pole frames turned in steps of ten
  degrees, the aims and turns a third of a step from any the planner asks, over long edges from 45
  to 85 degrees in three shapes and both orientations, and the three lenses above at 20,000
  directions and every five degrees of heading. A review's model at 20,000 directions, from 20 to 85
  degrees in four shapes either way round, found nothing open at 30% overlap. Asked the test's way
  at 20,000 directions, every pole frame turned in steps of five degrees, the 1671 plans laid from
  20 to 90 degrees in four shapes either way round, at overlaps of none, 10% and 30%, leave nothing
  open.
- The caps and zero overlap are closed at that resolution, which ADR 0075's consequences recorded
  as open. Roll at the cone's edge is still not part of the check, so ADR 0075's roll figures
  stand.
- A capture saved before a plan changes and resumed after it is filed under the new plan. `Resume`
  remakes the plan from the stored spec (ADR 0029) and checks only that each stored cell id still
  exists, so where a lens's plan changed, its ids now name other cells: for 16:9 held upright at 66
  degrees, 38 of 39 point more than 4 degrees from where they did. The page's lens is unchanged by
  this ADR, as it was not by ADR 0075. Making a resumed capture keep its plan is open.
- `LaysNoMoreCellsThanItNeeds` pins the page's 32 and 38 cells and holds 16:9 to 64, so a
  shrink coarser than the check needs fails a test rather than costing cells unseen.
- The planner's own tests take about 23 seconds in a debug build, where they took one.

## Rejected alternatives

**The rules on their own.** They cleared the sweep they were tuned against and failed another. A
rule's completeness is a claim about every lens, and the only evidence for it was a sweep.

**Refuse overlap below a floor.** It closes zero overlap and nothing else: the caps and the narrow
lenses failed at 30%.

**Check the plan only in the tests.** That is what ADR 0075 did, and every round of review found a
lens the tests had not tried.
