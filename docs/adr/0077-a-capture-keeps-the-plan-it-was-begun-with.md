# 0077 — A capture keeps the plan it was begun with

**Status:** accepted; amends [0029](0029-a-session-is-resumed-by-the-manager-that-owns-one.md) and
[0076](0076-the-planner-checks-its-own-plan.md), whose open item on resuming it closes

## Context

ADR 0029 replanned a resumed session from the spec and lens its document stored, never from the
camera in front of the phone, because node ids are indices into a particular tessellation. That
held only while the planner did not change. ADR 0075 changed it, and ADR 0076 changed it again:
for 16:9 held upright at 66 degrees the plan went from 39 cells to 64, and from the second cell on
every id named a different direction. `Resume` checked only that each stored candidate's id still
named a cell, and ids 1 to 39 all did, so a capture begun before the deploy and resumed after it
came back with its frames filed under cells nobody had photographed and the cells actually shot
reading as holes. Nothing refused it. The page's own lens was not spared: ADR 0076 left its plan as
it was, but ADR 0075 took it from 28 cells to 32 held upright, and 26 of the 28 ids moved more than
4 degrees, so a capture on the page's lens begun before ADR 0075 shipped and resumed after it was
misfiled the same way. No such capture has been reported.

The spec is the wrong thing to keep. It names the sphere the user asked for; the plan names the
sphere they are capturing, and only the second is what the candidates were filed against.

## Decision

**The session document carries the plan's cells, and `Resume` takes them as they stand.**
- **Version 3** of the document writes one `cell` line per node: its id, ring, target orientation
  to 17 significant digits, and acceptance cone. A resumed plan is the begun one bit for bit.
- **A cell is checked as it is read**, since no planner vouches for it: an identity a counter could
  issue, a ring index not below zero, a target that is a unit quaternion, a cone above zero and no
  wider than the spec's, and one cell per id. The page draws a cell from its target without
  normalising it, and a cone wider than was asked for arms a burst on a cell the camera is nowhere
  near. A version-3 document with no cells is refused, since no plan has none.
- **The stored spec is checked without a planner**: a field of view a lens has, an overlap short of
  one, and a cone narrower than half the frame. The planner was what refused a spec that is not a
  lens, none runs on this path, and a later one might refuse what the one that laid the cells
  accepted. The page sizes its overlay from this spec. A version-2 document is held to the rule
  too, and to its planner.
- **Every refusal of a stored document's own bytes is `Unsupported`**, a planner's refusal of a
  version-2 spec and an empty replan among them, since the planner is a pure function: the core's
  word for "this build cannot act on these bytes", since pressed again the resume would be refused
  again. What the page does with it is the page's (`resume.ts`).
- **The plan carries the spec it was asked for**, whatever a planner hands back with its cells,
  so the spec the page reads and the spec written beside the cells are one copy.
- **`Begin` holds the spec it plans from to the same rule**, so that what it writes a resume can
  read whatever planner is wired in: one looser than the rule began captures that could not be
  resumed. After the planner, whose own refusal says more, and before the tier is emptied.
- **A plan of no cells is refused**, by `Begin` and by a version-2 replan alike: nothing forbids a
  planner to return one, and its document is one the decoder would refuse.
- **Version 2 is still read, and replanned** from its spec and lens as every resume was. Refusing
  it would strand every capture in flight when this build arrives, and most of those replan
  correctly: every one begun since ADR 0076 shipped, and on the page's lens every one begun since
  ADR 0075. A version-2 document carrying `cell` lines is refused: the version says what shape the
  rest is.

The spec is still written, and still returned with the plan. It is what a version-2 document is
replanned from, and the record of what was asked for.

## Consequences

- A capture resumed under a later build is captured against the plan it began with, whatever that
  build's planner lays. `KeepsThePlanItWasBegunWithWhenThePlannerHasSinceChanged` resumes under a
  planner that lays the same ids turned a fifth of a radian, and requires every cell back bit for
  bit.
- Captures begun before this build and resumed after it are replanned, so the window ADR 0076
  recorded stays open for them and closes for every capture begun from now on.
- A version-3 resume does not plan, so ADR 0076's planning time is paid at `Begin` alone.
- The document grows by 76 bytes a cell for the page's lens, 2.4 KB in all, and by more for a long
  lens or a wide cone, whose ids, rings and cones print longer: the 1946-cell plan the page's lens
  lays at a 22.8 degree cone adds 216 KB, 111 bytes a cell. It is rewritten after every burst.
- A plan's cells are now a stored format. A field added to `CoverageNode` that a resume needs is a
  new document version, the way the tier line was (ADR 0035).
- `DecodeSessionDocument` now starts from an empty document whatever it is handed, since a cell or
  candidate left from an earlier read would make a document that lost its cells look whole.

## Rejected alternatives

**A fingerprint of the plan, refused on mismatch.** It turns misfiling into a refusal, which is
honest and still loses the capture: a planner change would end every capture in flight, and the
next planner change would do it again.

**A planner version in the spec.** The same refusal, one step removed: it says the plan changed
without saying what it was, and a planner change that forgot to bump it would misfile exactly as
before.

**Refusing version 2.** It closes the window for the captures already in it by discarding them,
including every capture on the page's lens, which replans correctly.
