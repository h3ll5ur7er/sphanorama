# 0075 — A burst's frames are taken only on its cell

**Status:** accepted

## Context

The first preview built on a phone (a Pixel 9 Pro XL, of a room) had gaps, and turning the phone
slightly while a burst was being taken widened them. Two places could have left them: the plan,
whose cells might not overlap enough for the frames a phone really takes, and the capture, which
might file frames under cells they were not aimed at.

**The plan has a margin, and it holds for what the page asks.** A model of the rings planner,
checked against 20,000 directions spread evenly over the sphere, used the page's spec: 66 degrees
on the long edge of a 4:3 frame held upright, 30% overlap, a 4 degree cone. That plan has 28 cells.
The model counts a direction covered when it falls inside the rectangle a camera images, not inside
a box of yaw and pitch angles, which is what the planner's own test checked and which credits each
frame with corners no camera has. Taking each cell's frame independently, and asking whether *any*
choice of frames could miss a direction, it found:

| What the frames do | Directions a capture could miss |
| --- | --- |
| Aimed anywhere inside the 4 degree cone, level | none |
| Aimed at the cell's centre, rolled up to 30 degrees either way | none |
| Aimed at the centre, rolled any amount | 0.6% |
| Anywhere in the cone, rolled up to 10 degrees | 0.16% |
| Anywhere in the cone, rolled up to 15 degrees | 0.56% |
| Anywhere in an 8 degree cone, level | 3.0% |

Frames taken at random within those bounds, rather than at the worst choice, missed nothing in
twenty trials of any row up to a 4 degree cone with 20 degrees of roll. The lens has a margin too:
with the plan made for 66 degrees and the frames really 54 degrees wide on the long edge, nothing is
missed; at 50 degrees, 0.7% is, and at 46 degrees, 4.1%. Google gives the Pixel 9 Pro's main camera
an 82 degree diagonal, about 70 degrees on the long edge of a 4:3 frame, though a browser stream
that crops it would be narrower and none has been measured. The first two rows are now tests against
the planner itself, which is where the model's numbers stop being a model.

So a level phone aimed inside the cone covers the sphere, and the plan cannot account for gaps
large enough to see in a preview of a room.

**The capture had no such bound.** `ArmBurst` refuses a cell the camera is not aimed at, because a
burst records whatever the camera sees and the cell is only a name to file it under (ADR 0041). It
asked that once. A burst is five frames over about half a second, and `AdvanceBurst` checked only
that each frame's pose was well formed. A phone that turned during the burst filed what it turned to
under the cell it had left. The pose was recorded honestly, so the build placed those frames where
they really looked, and the cell's own direction was left to whatever overlapped it. That matches
the report: turning during a burst widened the gaps.

The same capture predates ADR 0074, when an indoor compass could carry the heading 20 degrees in a
two-second dwell. Frames aimed by a pose that wrong land in the wrong place whatever the burst does.
That is now fixed, so how much of the report each cause explains is not known, and no capture since
has been looked at.

## Decision

**Every frame of a burst is taken on the condition the arm checked: the camera inside the armed
cell's cone.** When a frame falls due with the camera outside it, the burst is dropped. Nothing it
took is kept, the locks are released, and the tick answers `TooFast` rather than failing. The user
moved, which is theirs to correct, and a failed tick reads on the page as guidance having stopped
working. The cell is still wanted, so the dwell starts again once the camera is back on it.

`TooFast` was in the enum and nothing produced it. It is the tick a burst was dropped, an edge like
`CellDone`, and the page says which cell, that it moved while capturing, and to hold still on it
again.

The check is the arm's own cone, not a wider one. The dwell already required two seconds inside it,
and a cone that let a burst wander further than the arm allows would make "the reticle is closed"
and "this frame counts" two conditions again. The direction and cone are kept from the arm, since
the plan does not change during a session.

Roll is not bounded. On its own it costs the plan nothing up to 30 degrees, and with an off-centre
aim a few tenths of a percent at worst. Refusing a burst for it would add a correction to every cell
for a margin no random capture reached.

## Consequences

- A user who moves during a burst loses it and holds the cell for another dwell, about two seconds,
  where before the burst finished and was filed under the cell anyway. A user who cannot hold within
  4 degrees for half a second cannot capture that cell at all.
- The plan's margin is now written down, and it is not unlimited. A lens narrower than about 54
  degrees on the long edge opens gaps, and the page still assumes 66 degrees: the lens a capture
  fits (ADR 0066, ADR 0067) is not yet fed back into the next plan. A phone held at the cone's edge
  and rolled can miss a few tenths of a percent of the sphere, and nothing reports it.
- How much of the field report this explains is unmeasured. Gaps a capture leaves for any reason are
  still invisible while capturing; finding them from where frames really landed is separate work.
- `CaptureSessionManager::OnMotion` and `GuidanceAction` say what `TooFast` means, and UC-1 in
  `docs/03-architecture.md` shows the drop.

## Rejected alternatives

**Plan for the worst case.** Laying cells so that any aim in the cone at any roll still covers
everything takes 45 cells where the page's plan has 28. Fewer will not do, because rings round to a
whole number: two degrees less of vertical field takes the plan from 5 rings to 7. That is 60% more
holding still for a worst case that random frames never reached, against gaps whose likelier causes
were the burst and the compass.

**Bound roll at the arm.** It closes the last few tenths of a percent and asks the user to level the
phone on every cell, including the ones where it costs nothing.

**Keep the burst armed and skip frames while the camera is off the cell.** The burst would resume
when the user came back, but it would hold the exposure lock and say `Firing` for as long as they
stayed away, and nothing bounds that.

**Fail the tick.** It is what a broken pose does mid-burst, and it puts "guidance failed" on the
page for a user who only moved.
