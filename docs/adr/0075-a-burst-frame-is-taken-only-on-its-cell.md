# 0075 — A burst's frames are taken only on its cell, and a ring closes where it is widest

**Status:** accepted

## Context

The first preview built on a phone (a Pixel 9 Pro XL, of a room) had gaps, and turning the phone
slightly while a burst was being taken widened them. Two places could have left them: the plan,
whose cells might not overlap enough for the frames a phone really takes, and the capture, which
might file frames under cells they were not aimed at. Both did.

**The capture had no bound on a burst.** `ArmBurst` refuses a cell the camera is not aimed at,
because a burst records whatever the camera sees and the cell is only a name to file it under
(ADR 0041). It asked that once. A burst is five frames over about half a second, and `AdvanceBurst`
checked only that each frame's pose was well formed. A phone that turned during the burst filed what
it turned to under the cell it had left. The pose was recorded honestly, so the build placed those
frames where they really looked, and the cell's own direction was left to whatever overlapped it.

**The plan was complete for the page's lens by rounding.** The page plans for 66 degrees on the long
edge of a 4:3 frame held upright, with 30% overlap and a 4 degree cone, and a burst is armed
anywhere inside that cone. Each ring was spaced by its circumference at its centre. The two rings at
45 degrees needed 7.002 cells each and were given 8, and with 8 a level frame anywhere in the cone
covered the sphere. A tenth of a degree more lens gave 7, which did not, and a 3:2 stream's narrower
frames did not close the ring with 8.

The figures come from a model of the planner checked against 20,000 directions spread evenly over
the sphere, counting a direction covered when it falls inside the rectangle a camera images. The
worst case is exact for one direction: each cell's frame is taken once and independently, so a
direction is a hole in some capture whenever every cell has a frame that misses it. Aims are 360
points around the cone's edge, and a worst case over sampled aims only rises with more of them.
"Level" means level against gravity.

| Plan spaced at the ring's centre (28 cells for the page) | Directions a capture could miss |
| --- | --- |
| The page's lens, anywhere in the 4 degree cone | none |
| 66.1 degrees on 4:3 (26 cells), anywhere in the cone | 0.30% |
| 66 degrees on 3:2 (29 cells), anywhere in the cone | 0.31% |
| Planned for 66, frames really 63 degrees, anywhere in the cone | 0.09% |
| Planned for 66, frames really 54 degrees, anywhere in the cone | 7.9% |

Frames taken at random rather than at the worst choice did no better at 54 degrees: every one of
twenty trials missed something. An earlier version of this ADR said the plan held down to 54
degrees, which was measured with every frame aimed at its cell's centre, not anywhere in the cone. A
review re-measured it.

Google gives the Pixel 9 Pro's main camera an 82 degree diagonal, about 70 degrees on the long edge
of a 4:3 frame, so on that phone the frames were probably wider than the plan assumed. A browser
stream that crops would be narrower, and none has been measured. The same capture also predates ADR
0074, when an indoor compass could carry the heading 20 degrees in a two-second dwell, and frames
aimed by a pose that wrong land in the wrong place whatever the plan does. How much of the report
each cause explains is not known.

## Decision

**Every frame of a burst is taken on the condition the arm checked: the camera inside the armed
cell's cone.** When a frame falls due with the camera outside it, the burst is dropped. Nothing it
took is kept, the locks are released, and the tick answers `TooFast` rather than failing. The user
moved, which is theirs to correct, and a failed tick reads on the page as guidance having stopped
working. A camera that refuses to drop its locks still fails the tick, as it does on the tick a
burst fills, because a camera left locked is the worse problem and the edge would hide it. The cell
is still wanted, so the dwell starts again once the camera is back on it.

`TooFast` was in the enum and nothing produced it. It is the tick a burst was dropped, an edge like
`CellDone`. The page says the burst was lost to moving and names no cell: the line is held past the
tick it is true on, by which time guidance may name another.

The check is the arm's own cone, not a wider one. The dwell already required two seconds inside it,
and a cone that let a burst wander further than the arm allows would make "the reticle is closed"
and "this frame counts" two conditions again. The direction and cone are kept from the arm, since
the plan does not change during a session.

**Each ring is spaced by its circumference where its band is widest**, which is the band's edge
nearer the horizon, and a ring at a pole is one cell. A frame on a ring reaches toward the horizon
as well as toward the pole, and its neighbours have to close the circle at both edges. The page's
plan grows from 28 cells to 32 held upright, and from 32 to 38 held sideways.

| Plan spaced at the ring's widest edge | Directions a capture could miss |
| --- | --- |
| 60, 66, 66.1, 67 or 70 degrees on 4:3, or 66 on 3:2 or 16:9, anywhere in the cone | none |
| Planned for 66, frames really 58 degrees, anywhere in the cone | none |
| Planned for 66, frames really 56 degrees, anywhere in the cone | 0.88% |
| The page's lens with no overlap at all, on the cell's centre | none |
| The page's lens on the cell's centre, rolled up to 25 degrees | none |
| The page's lens on the cell's centre, rolled any amount | 0.69% |
| Anywhere in the cone, rolled up to 10 degrees | none |
| Anywhere in the cone, rolled up to 20 degrees | 1.4% |
| Anywhere in the cone, rolled any amount | 10% |
| Anywhere in an 8 degree cone, level | 0.65% |

The first, fourth and fifth rows are tests against the planner itself, over the lenses the first
names except 70, which is where the model's numbers stop being a model. The tests tilt each frame in
its cell's own frame rather than keeping it level, which leaves it up to 4 degrees off level; both
give none on those rows. The fourth holds for the page's lens and the old test lens and is not
general: across 276 lenses a review swept, 62 still leave holes at zero overlap with every frame on
its cell's centre, against 179 spaced at the centre and none at 30% overlap. In the cone at zero
overlap the page's lens misses 7.15%.

Roll is not bounded. Frames taken at random, rolled any amount anywhere in the cone, missed between
0.001% and 0.008% on average in five batches of twenty trials, and between 0.01% and 0.06% in each
batch's worst trial; a review's batches reached 0.12%. Up to 30 degrees of roll, misses were rare: 4
trials of a review's 1,000 missed 0.01%. The worst case needs every cell rolled the wrong way for
one direction.

## Consequences

- A user who moves during a burst loses it and holds the cell for another dwell, about two seconds,
  where before the burst finished and was filed under the cell anyway. A user who cannot hold within
  4 degrees for half a second cannot capture that cell at all.
- Four more cells to hold still on, held upright, and six more held sideways.
- A phone rolled at the cone's edge can still leave a hole, up to a tenth of the sphere in the worst
  case and nothing like it in practice, and nothing reports it. A lens narrower than about 58
  degrees opens gaps, and the page still assumes 66: the lens a capture fits (ADR 0066, ADR 0067) is
  not yet fed back into the next plan.
- The caps beside the poles are still not closed for every lens. Rings are laid as though the single
  frame at a pole reached half its vertical field in every direction, and across its short edge it
  reaches less, so some lenses near the ones tried leave holes at 73 to 75 degrees for frames in the
  cone: held upright, 16:9 from 64.3 to 65.5 degrees and 3:2 from 42.9 to 44.5, and at the page's 66
  degrees any stream wider than 16:9. A review swept long edges from 20 to 90 degrees: 1182 lenses
  failed spaced at the rings' centres and 670 at the widest edge. It predates this ADR and is open,
  with the lenses zero overlap leaves holes for.
- How much of the field report this explains is unmeasured. Gaps a capture leaves for any reason are
  still invisible while capturing; finding them from where frames really landed is separate work.
- `CaptureSessionManager::OnMotion` and `GuidanceAction` say what `TooFast` means, and UC-1 in
  `docs/03-architecture.md` shows the drop.

## Rejected alternatives

**Plan for the worst case at any roll.** Shrinking each field of view by part of the cone before
spacing the rings at their centres took 45 cells for the page's lens, where the widest edge takes
32, for a worst case random frames came nowhere near: the worst trial any batch recorded missed a
tenth of a percent.

**Bound roll at the arm.** It closes the worst case and asks the user to level the phone on every
cell, including the ones where it costs nothing.

**Keep the burst armed and skip frames while the camera is off the cell.** The burst would resume
when the user came back, but it would hold the exposure lock and say `Firing` for as long as they
stayed away, and nothing bounds that.

**Fail the tick.** It is what a broken pose does mid-burst, and it puts "guidance failed" on the
page for a user who only moved.
