# 0076 — The planner checks its own plan

**Status:** accepted; amends [0075](0075-a-burst-frame-is-taken-only-on-its-cell.md)

## Context

ADR 0075 spaced each ring where its band is widest, and recorded what that still left open. Rings
were laid as though the single frame at a pole reached half its vertical field in every direction,
and across its short edge it reaches less, so some lenses near the ones tried left holes at 73 to 75
degrees. At zero overlap, 62 of 276 lenses left holes with every frame on its cell's centre. And
every claim of completeness rested on chosen lenses, each of which passed while a lens a fraction of
a degree away did not.

A sweep of the real planner made the third point general. With pole frames taken at any heading,
since guidance asks for no roll within five degrees of a pole (ADR 0072), and frames anywhere in a 4
degree cone at 30% overlap, 29 of 141 long edges from 20 to 90 degrees failed on 4:3, 35 on 3:2 and
47 on 16:9. Most were narrow lenses, where a cone of 4 degrees on each side of a shared edge eats an
overlap of 30%. The rest were the caps.

Rules for each failure were tried next: steps capped at the field less two and a half cones, and
rings added until a pole frame's short half-edge reached halfway to the next ring. They cleared
that sweep. Asked again with an offset, denser sampling, they left directions open. Three cones
instead of two and a half failed lenses that two and a half had passed, because ring and cell
counts round, so a margin tuned until a sweep is clean is complete by rounding again, one level up.

## Decision

**`RingsCoveragePlannerEngine::Plan` checks the plan it lays, and lays it tighter until the check
passes.**
- **The check.** For each of 4000 directions spread evenly over the sphere, the question is
  whether some cell's frame covers it whether that frame is aimed at the cell's centre or at any of
  16 points around its cone's edge. For one direction this is the worst case exactly, since each
  cell's frame is taken once.
- **The frame it asks about is a degree smaller than the lens on every side.** Asked of the lens
  itself, plans complete at the 4000 directions left two in 5000 open at another sampling. The
  degree is what a real frame spends covering the directions and aims between the ones asked.
- **When the check fails,** both steps shrink by 5% and the rings are laid again. The bound on cells
  a session can hold ends the search, so a lens that never closes is refused rather than hung on.
- **A cone too wide for its lens is refused**: one at least half the narrower field, less the
  degree. A frame aimed at its edge would not see its own cell's centre, and no spacing closes a
  sphere of those.

The steps start where ADR 0075 left them: the overlap's, with each ring spaced where its band is
widest. The rules tried above are not kept. Over 102 phone lenses, with pole headings asked as
well, the check alone laid 5207 cells and the check with the rules 5217.

Pole frames at any heading are not part of the check. The degree and the aims round the cone
already close the caps: the engine without that question passed the tests, which do ask it, and
1010 combinations of lens and orientation in a wider sweep.

## Consequences

- The page's lens plans as before: 32 cells held upright and 38 held sideways, complete as first
  laid. Every 4:3 lens from 60 to 75 degrees plans the cells it did before this ADR.
- Lenses that left holes cost cells now. 16:9 held upright at 66 degrees goes from 39 cells to 64,
  3:2 at 43.5 degrees from 80 to 111, and 4:3 at 30 degrees from 169 to 254. Shrinking the two
  steps in turn rather than together was tried, and saved less than 1%.
- Planning takes about 5 milliseconds for the page's lens at `-O2`, and 37 for a 30 degree lens.
  It happens once, at `Begin` or `Resume`.
- Completeness is at the resolution checked, with the degree as its margin. The tests check plans at
  a denser and offset sampling: 5000 directions, 24 aims and pole frames turned in steps of ten
  degrees, over long edges from 45 to 85 degrees in three shapes and both orientations. A model at
  20,000 directions found nothing open for 4:3, 3:2, 16:9 or square frames, upright, sideways or at
  zero overlap.
- The caps and zero overlap are closed, which ADR 0075's consequences recorded as open. Roll at the
  cone's edge is still not part of the check, so ADR 0075's roll figures stand.
- The planner's own tests take about 18 seconds in a debug build, where they took one.

## Rejected alternatives

**The rules on their own.** They cleared the sweep they were tuned against and failed another. A
rule's completeness is a claim about every lens, and the only evidence for it was a sweep.

**Refuse overlap below a floor.** It closes zero overlap and nothing else: the caps and the narrow
lenses failed at 30%.

**Check the plan only in the tests.** That is what ADR 0075 did, and every round of review found a
lens the tests had not tried.
