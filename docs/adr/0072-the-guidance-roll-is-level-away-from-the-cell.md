# 0072 — The guidance roll is measured against the cell near it and against level away from it

**Status:** accepted

## Context

`CaptureGuidance::rollErrorDeg` is drawn as the horizon on every tick where the pose is known,
`Seek` included. Both coverage planners computed it as `RollBetween(current, target)`: the twist
left after the shortest turn from the cell's view to the phone's. Near the cell that is the right
number — it is the roll the frame will have once the phone is aimed. Away from the cell it is not a
guide at all, which a review of PR #92 measured:

- It winds two full turns around the point behind the cell. A level phone circling it reads -720
  degrees per circuit at every radius tried, so half a degree of aim near there can flip the horizon
  by 180, and a level phone fifteen to thirty degrees from it can read 90.
- Callers reach that region. Outside every cone the target is the nearest *missing* cell, and near
  the end of a capture that can be anywhere, including behind the user.
- Off the horizon the shortest turn tips the horizon as it goes, so a level phone reads rolled
  against a level cell beside it: 7.5 degrees at fifteen of azimuth from a cell thirty up, and nearly
  40 on the upper rings.

People turn a phone by yaw and pitch, keeping it level, not along the shortest arc — so away from
the cell, what they need from the horizon is how far they are from level.

## Decision

**Within 5 degrees of the target, the roll is `RollBetween(current, target)`; beyond 15 it is
`RollFromLevel(current)`, the roll against the orientation that looks the same way with the horizon
flat; between the two it is a linear blend of the two, taken the short way round.** One function,
`GuidanceRollDeg` in the coverage planner component, gives it for both engines, because it is one
policy. Cells are level, so at the cell the two references agree exactly, and the roll owed once
aimed is unchanged. The separations are between where the phone and the cell look, not between
whole attitudes, which would count the roll itself as distance.

The band is narrow because the two references part fast. Within 15 degrees of a cell they differ
by 9.1 degrees at thirty up, 27.7 at sixty, 40.3 at 67.5 and 80.0 at seventy-five, and further out
by up to half a turn, so a switch at any separation would jump the horizon by that much.
Interpolating two angles is continuous only while they stay short of half a turn apart: around the
point behind the cell one reference winds twice and the other not at all, and a blend between maps
of different winding is no more continuous than a switch.

**Near the poles, level fades out: within 15 degrees of straight up or down the roll from level is
scaled down linearly, to nothing within 5. And a cell within 15 degrees of a pole is measured
against that faded level alone.** Level has no meaning at a pole, and the roll from it spins faster
the closer the phone gets — at a tenth of a degree from straight up, a twentieth of a degree of aim
can turn it by half a turn. The cell rule is the same winding argument again: circled round a
pole, the cell's own reference turns once and level does not, so for a cell close enough that the
circle lies in the band no blend between them is continuous. The planner lays a cell at each pole
by default, and against its own reference a level phone ten degrees off it read +90 at one azimuth
and -90 a twentieth of a degree further round. A cell straight up or down therefore owes no roll at
all, which is also the truth about it: turning a frame about a view straight up changes nothing a
stitch needs.

Measured over 2.1 million steps of a twentieth of a degree, on random walks of the whole
orientation past cells at every elevation a plan of one to nine half-rings lays, poles included:
for a phone within a quarter-turn of level the worst step is 0.87 degrees of roll and none is over
2. All 116 steps over 2 were a phone rolled 147.8 degrees or more from level within 15 degrees of a
pole — leaning back past straight up, or upside down — where the faded level crosses its half-turn.

## Consequences

- **A level phone reads level wherever it points**, except within 15 degrees of a cell off the
  horizon: within 5 it reads the roll owed after turning onto the cell, the number the frame will
  have, and between 5 and 15 a share of it that shrinks to nothing at 15.
- **A cell at a pole asks for no roll.** Before, it asked the user to turn the phone to whichever
  heading the planner happened to give it — up to a half-turn, for nothing.
- **Near a pole the horizon fades.** Within 15 degrees of straight up or down a rolled phone reads
  less than its roll, and within 5 it reads none.
- **The unsteady spot moves rather than disappears.** Every roll defined over all orientations has
  one, and this one sits on a phone rolled most of a half-turn from level near a pole. Before, it
  sat behind the target and around the pole cells, which a capture reaches on every sphere.
- **`rollErrorDeg` changes meaning off the cell and at the poles.** The contract comment says so,
  and so do the shell's two comments that described it. Acceptance never read roll, so no capture
  decision moves; the shell draws the number and nothing more.
- `NullCoveragePlanner.ReportsTheRollLeftAfterTheShortestTurnOntoTheCell` pinned 7.532 at a point
  13 degrees off the cell, which is now inside the blend; the near-cell behaviour is pinned at four
  degrees instead, against both engines. `CoveragePlanner.RollDoesNotCountAsBeingOffTarget` took
  the plan's first cell, which is the nadir; it takes one on the horizon now.

## Rejected alternatives

**A hard switch at a separation (90 degrees was proposed).** The jump at the switch is up to 180
degrees from 45 out, and 80 at 15. **A blend over 15 to 45 degrees**, the first version of this
decision: measured, it jumps 161 degrees in one step where the references are half a turn apart
inside the band, which is the winding argument above showing up as a horizon spinning. **A
`rollKnown` flag beside the roll**, so the page could hide the horizon away from the cell: a contract
field and a shell change, to throw away a number that level-here roll makes meaningful.

**Level alone around a pole cell, unfaded.** Steady on the way in, but level is singular at the cell
itself, so the horizon would spin exactly where the user holds still for the dwell. **The pole
cell's own reference throughout.** Steady at the cell, but a level phone coming up from the side
opposite the cell's heading reads half a turn — the original defect, moved to the zenith. Both are
the two ends of the winding argument; fading level to nothing is the one answer that is steady at
the cell and level away from it, and what it costs is the phone rolled near a half-turn.
