# 0084 — Gaps are found after a build, and filled by new cells

**Status:** accepted; the first of three steps, and only `FindGaps` is built

## Context

A field capture came back with holes in the sphere that nobody saw while capturing. Every cell had
been shot, and the review strip said the sphere was complete.

`Evaluate` counts a cell as covered when a frame is filed under it. Where that frame points does
not enter into it. Since ADR 0076 that is enough under one assumption: the plan is checked to cover
the sphere for any frame aimed inside its cell's cone, through the lens the plan was laid with. So
a gap can only come from that lens being wrong, or from the pose a frame was filed at being wrong.
The page's lens is a 66-degree guess (ADR 0067). Poses drift (ADR 0074).

Neither shows during the capture. A live check would use the same lens and the same poses that are
wrong. Both show after a build: `Refine` fits the focal length where the capture's loops see it
(ADR 0066) and places each frame from its pixels (ADR 0065).

Two choices were put to the maintainer:
- Where to detect gaps: during the capture, after a build, or both.
- How to fill them: re-shoot the nearest cells, or add new cells.

They chose after a build, and new cells.

## Decision

- **A gap is judged from where the solve placed the frames.** `ICoveragePlannerEngine::FindGaps`
  takes the plan, each frame's solved rotation and its lens as the frame was held (`PlacedFrame`),
  and the lens fill frames will be taken through.
  - It answers which parts of the sphere no frame covers, and where to aim to fill each.
  - V4 already lists hole detection as its own (`02-volatility-map.md`), so no component is added.
- **The directions asked are the plan's.** `FindGaps` asks 20,000 directions spread evenly over the
  sphere, about a degree and a half apart, and 720 on the horizon, as the plan's own check does
  (ADR 0076).
  - A band answers only for the elevations its rings aim at. One of only its horizon ring answers
    for the horizon alone, and no point of the spread lands there.
  - The horizon's directions are asked, and weigh nothing in a gap's share of the sphere.
  - A direction is covered when one frame's `Project` lands inside that frame, for it and for
    eight directions 1.1 degrees around it. No point of the sphere is more than about 1.012
    degrees from an asked direction, and the ring's hull holds every point that near. So a point
    left open always has an open direction that near, and so does every target's share of a gap.
    Asked of the directions alone, a review found filling left slivers open between them.
- **A target is one a fill frame covers from anywhere in the cone.** Open directions are gathered
  into targets of one reach each.
  - The reach is the circle inscribed in the fill frame, less the plan's cone, less the 1.1
    degrees between directions. The circle is the nearest of the frame's four edges to its axis,
    read through `Unproject`, so an off-centre or distorted lens is read as it is: half its field
    across overstated the near side.
  - A gap wider than one frame comes back as several targets, so aiming at every target closes it.
  - Each target is level, since nothing about a gap says which way up to hold the phone.
  - A fill lens whose reach is below the spacing of the directions, a degree and a half, is
    refused: every open direction would be its own target.
- **Next, the build reports its gaps.** `PanoramaBuildManager` asks `FindGaps` after `Refine`,
  through the fitted lens, and the build's progress says where the gaps are. This is not built yet.
- **Then, filling appends cells to the capture's plan.** `CaptureSessionManager` appends a cell at
  each target to the stored plan and resumes, so the reticle leads there. Re-shooting the nearest
  cell would not fill a gap that sits between cells shot through too narrow a lens. ADR 0077 keeps
  a capture's plan as begun, and an appended cell is a new cell of it, not a replanned one. This is
  not built yet either.

## Consequences

- **The plan's overlap hides a wrong lens for longer than expected.** On the default 66 by 50 plan,
  with every frame aimed at its cell's centre:
  - Frames 50 degrees across leave no gap. Aimed at their cones' edges, they do.
  - Frames 46 across leave seams thinner than the distance between directions, 0.7% of the
    sphere, and 42 by 32 leave 5.0%.
- **A seam thinner than about 2.2 degrees is a gap.** A direction near the meeting of two frames
  that overlap by less than the ring's width is covered by neither with its ring around it, so it
  is reported open although every point there is imaged. That is the cost of never missing a
  sliver. A seam that thin is also one a stitch has little overlap to place.
- **Targets overlap, and can be many.** They are gathered greedily, not as a minimum cover, so a
  large gap asks for more fill frames than a careful layout would. With nothing placed on the
  default plan, the plan's own 66 by 50 lens asks for 63 targets, a 30-degree fill lens for 276,
  and the narrowest accepted, about 15 degrees, for 7,289: more than the 2,000 cells a plan may
  hold. Appending fill cells has to bound that.
- **The solve's world is the capture's.** The priors decide which way a reconstruction faces
  (ADR 0065), so a target is in the same frame as the plan's cells, give or take the solve's error
  against the priors. A sensor drifting badly enough to move that by more than a cone would aim a
  fill cell off its gap. A rebuild after filling shows any gap that is left.
- **The cost.** Each call projects up to 20,720 directions, and nine times that where a frame
  covers one, against the frames near them; each target asks only the directions within its reach
  in elevation. In a native debug build on the default plan: 44 ms with a frame at every cell, and
  with nothing placed 68 ms for a 30-degree fill lens and 302 ms for the narrowest. Scanning every
  open direction per target, as the first version did, took 6.2 s at a 2-degree reach, as a
  review measured.
- The null planner refuses with `Unsupported`, since one cell is no tessellation to judge the
  sphere against.

## Rejected alternatives

**Judge gaps during the capture, from the sensor's poses and the page's lens.** It would be live
and cheap. But it shares the very lens and poses a gap comes from, so by ADR 0076 it would find
only an unusual roll, and none of what the field report saw.

**Re-shoot the cells nearest each gap.** It keeps the plan fixed. But a gap between two cells shot
through too narrow a lens stays open, however often either cell is shot.

**Return gaps as regions, and let the capture manager place cells.** That puts the question of what
a fill frame covers in a second component. Coverage is V4's, and so is the reach of a frame.
