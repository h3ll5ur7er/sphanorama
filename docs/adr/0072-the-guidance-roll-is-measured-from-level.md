# 0072 — The guidance roll is measured from level, faded out near the poles

**Status:** accepted

## Context

`CaptureGuidance::rollErrorDeg` is drawn as the horizon on every tick where the pose is known,
`Seek` included. Both coverage planners computed it as `RollBetween(current, target)`: the twist
left after the shortest turn from the cell's view to the phone's. Near the cell that is a defensible
number — it is the roll the frame would have if the phone were turned onto the cell along that
shortest arc. Away from the cell it is not a guide at all, which a review of PR #92 measured:

- It winds two full turns around the point behind the cell. A level phone circling it reads -720
  degrees per circuit at every radius tried, so half a degree of aim near there can flip the horizon
  by 180, and a level phone fifteen to thirty degrees from it can read 90.
- Callers reach that region. Outside every cone the target is the nearest *missing* cell, and near
  the end of a capture that can be anywhere, including behind the user.
- Off the horizon the shortest turn tips the horizon as it goes, so a level phone reads rolled
  against a level cell beside it: 7.5 degrees at fifteen of azimuth from a cell thirty up, and nearly
  40 on the upper rings.

People turn a phone by yaw and pitch, keeping it level, not along the shortest arc — so what they
need from the horizon is how far they are from level.

## Decision

**The roll is `RollFromLevel(current)` — the roll against the orientation that looks the same way
with the horizon flat — scaled down linearly within 15 degrees of straight up or down, to nothing
within 5.** It does not read the target. One function, `GuidanceRollDeg` in the coverage planner
component, gives it for both engines, because it is one policy.

Cells are level, so aimed at the centre of a cell the reading is exactly the roll the frame will
have against it, for every cell at least 15 degrees from a pole. A cell nearer a pole than that is
asked for a share of its roll, and one at a pole for none: a frame looking straight up has no
horizon to keep, and turning it about its view changes nothing a stitch needs.

The fade is there because level has no meaning at a pole and the roll from it spins faster the
closer the phone gets: at a fortieth of a degree from one, a twentieth of a degree of aim can turn
it half a turn. Faded, a phone tipped over the zenith keeps a steady horizon.

Measured over 8 million steps of a twentieth of a degree, on random walks of the whole orientation
over the whole sphere: for a phone within a quarter-turn of level the worst step is 0.48 degrees
of roll and none is over 2. All 81 steps over 2 were a phone 179.7 degrees or more from level and
between 5 and 15 degrees from a pole — upside down, or tipped back past straight up — where the
faded reading crosses its half-turn. Because the target is not read, no change of target moves it.

## Consequences

- **A level phone reads level wherever it points**, near a cell or far from it, except within 15
  degrees of a pole, where any phone reads less than its roll, and within 5, where it reads none.
- **Off the centre of a cell the reading is level, not the twist left after the shortest turn onto
  the cell.** Inside the 4-degree acceptance cone the two differ by up to 2.3 degrees on a ring
  thirty up, 6.9 at sixty and 15.1 at seventy-five. A phone turned onto the cell by yaw and pitch
  arrives level, so level is the reading that agrees with how it got there.
- **A cell at a pole asks for no roll**, and a cell nearer a pole than 15 degrees asks for a share.
  Before, a pole cell asked the user to turn the phone to whichever heading the planner happened to
  give it — up to a half-turn, for nothing.
- **The unsteady spot moves rather than disappears.** Every roll defined over all orientations has
  one. This one is a phone turned a half-turn from level near a pole — upside down, or pitched back
  past straight up or down by 5 to 15 degrees, where the sign of the reading follows the sensor's
  jitter. Before, it sat behind the target, which a capture reaches on every sphere.
- **`rollErrorDeg` changes meaning.** The contract comment says so, and so do the shell's two
  comments that described it. Acceptance never read roll, so no capture decision moves; the shell
  draws the number and nothing more.
- `NullCoveragePlanner.ReportsTheRollLeftAfterTheShortestTurnOntoTheCell` pinned 7.532 against a
  cell, which nothing reports any more. `CoveragePlanner.RollDoesNotCountAsBeingOffTarget` took the
  plan's first cell, which is the nadir; it takes one on the horizon now.

## Rejected alternatives

**The cell's twist near it and level away from it, blended across 5 to 15 degrees** — what this
decision first said, through two review rounds on PR #96. The two references wind differently
around a pole, so no blend between them is continuous round the cells every plan lays there: a
level phone ten degrees off the zenith cell flipped from +90 to -90 in a twentieth of a degree.
Fading level near the poles and measuring cells near one against level alone repaired that, and the
second round found what was left, all of it the blend reading the target: wherever the target
changed between two missing cells less than thirty degrees apart the horizon jumped, 31 degrees for
a level phone on the narrow-lens plan and up to 52 on others; the cells of the ring at exactly 75
took one rule or the other by the last bit of a sine; and near the 72- and 75-degree rings, phones
rolled from about 95 degrees flipped. What the blend bought was the shortest-turn twist inside the
cone, which the consequence above prices.

**A hard switch between the two references at a separation (90 degrees was proposed).** The jump at
the switch is up to half a turn. **A wider blend, 15 to 45 degrees**: it jumped 161 degrees in one
step where the references were half a turn apart inside the band.

**Level unfaded.** Steady everywhere but at the poles, and at the poles it spins exactly where the
user holds still for the zenith cell's dwell. **A `rollKnown` flag beside the roll**, so the page
could hide the horizon where it is unreliable: a contract field and a shell change, to throw away a
number that is meaningful nearly everywhere.
