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
  sphere, about a degree and a half apart.
  - A band answers only for the elevations its rings aim at, as the plan's own check does
    (ADR 0076).
  - A direction is covered when some frame's `Project` lands inside that frame.
- **A target is one a fill frame covers from anywhere in the cone.** Open directions are gathered
  into targets of one reach each.
  - The reach is half the fill lens's narrower field, less the plan's cone, less the degree
    ADR 0076 spends between the directions it asks.
  - A gap wider than one frame comes back as several targets, so aiming at every target closes it.
  - Each target is level, since nothing about a gap says which way up to hold the phone.
  - A fill lens too narrow to cover from inside the cone is refused.
- **Next, the build reports its gaps.** `PanoramaBuildManager` asks `FindGaps` after `Refine`,
  through the fitted lens, and the build's progress says where the gaps are. This is not built yet.
- **Then, filling appends cells to the capture's plan.** `CaptureSessionManager` appends a cell at
  each target to the stored plan and resumes, so the reticle leads there. Re-shooting the nearest
  cell would not fill a gap that sits between cells shot through too narrow a lens. ADR 0077 keeps
  a capture's plan as begun, and an appended cell is a new cell of it, not a replanned one. This is
  not built yet either.

## Consequences

- **The plan's overlap hides a wrong lens for longer than expected.** On the default 66 by 50 plan:
  - With every frame aimed at its cell's centre, frames 46 degrees across leave no gap, and frames
    42 across leave 24 targets, about 2% of the sphere.
  - Frames 50 across leave none at the centres, but do leave gaps aimed at their cones' edges.
- **A gap smaller than the spacing can be missed.** Directions are about a degree and a half apart,
  so a sliver narrower than that can fall between them. Filling with the degree of margin covers
  the space between asked directions, as it does for the plan (ADR 0076).
- **Targets overlap.** They are gathered greedily, not as a minimum cover, so a large gap asks for
  more fill frames than a careful layout would. With nothing placed, it asks for dozens.
- **The solve's world is the capture's.** The priors decide which way a reconstruction faces
  (ADR 0065), so a target is in the same frame as the plan's cells, give or take the solve's error
  against the priors. A sensor drifting badly enough to move that by more than a cone would aim a
  fill cell off its gap. A rebuild after filling shows any gap that is left.
- Each call projects 20,000 directions against the frames near them. That is about 0.1 s natively
  in a debug build for a plan of 38 cells with nothing placed, and less with frames placed.
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
