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
aimed is unchanged.

The band is narrow because the two references part fast. Within 15 degrees of a cell they differ by
up to 30 degrees on high cells, and further out by up to half a turn, so a switch at any separation
would jump the horizon by that much. Interpolating two angles is continuous only while they stay
short of half a turn apart: a blend is a homotopy from a constant map of the circle to the identity,
and none exists. Within 15 degrees of every cell up to 80 off the horizon they stay well short of
it.

Measured over 16 million steps of a twentieth of a degree on random paths past random cells: 0.24
degrees of roll at worst per step away from the poles, and 19 steps over 2 degrees, all near
straight up or down.

## Consequences

- **A level phone reads level wherever it points**, except within 5 degrees of a cell off the
  horizon, where it reads the roll owed after turning onto it — the band where that number is the
  one the frame will have.
- **The unsteady spot moves rather than disappears.** Every roll defined over all view directions
  has one, and this one sits where level has no meaning: looking straight up or down while the
  target is more than 15 degrees away. Before, it sat behind the target, which a capture reaches
  more often than the zenith.
- **`rollErrorDeg` changes meaning off the cell.** The contract comment says so. Acceptance never
  read roll, so no capture decision moves; the shell draws the number and nothing more.
- `NullCoveragePlanner.ReportsTheRollLeftAfterTheShortestTurnOntoTheCell` pinned 7.532 at a point
  13 degrees off the cell, which is now inside the blend; the near-cell behaviour is pinned at four
  degrees instead, against both engines.

## Rejected alternatives

**A hard switch at a separation (90 degrees was proposed).** The jump at the switch is up to 180
degrees from 45 out, and 30 at 15. **A blend over 15 to 45 degrees**, the first version of this
decision: measured, it jumps 161 degrees in one step where the references are half a turn apart
inside the band, which is the homotopy argument above showing up as a horizon spinning. **A
`rollKnown` flag beside the roll**, so the page could hide the horizon away from the cell: a contract
field and a shell change, to throw away a number that level-here roll makes meaningful.
