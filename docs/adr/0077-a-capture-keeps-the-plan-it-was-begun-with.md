# 0077 — A capture keeps the plan it was begun with

**Status:** accepted; amends [0029](0029-a-session-is-resumed-by-the-manager-that-owns-one.md)

## Context

ADR 0029 replanned a resumed session from the spec and lens its document stored, never from the
camera in front of the phone, because node ids are indices into a particular tessellation. That
held only while the planner did not change. ADR 0075 changed it, and ADR 0076 changed it again:
for 16:9 held upright at 66 degrees the plan went from 39 cells to 64, and from the second cell on
every id named a different direction. `Resume` checked only that each stored candidate's id still
named a cell, and ids 1 to 39 all did, so a capture begun before the deploy and resumed after it
came back with its frames filed under cells nobody had photographed and the cells actually shot
reading as holes. Nothing refused it. The page's own lens planned the same on all three builds,
which is the only reason no capture is known to have been hurt.

The spec is the wrong thing to keep. It names the sphere the user asked for; the plan names the
sphere they are capturing, and only the second is what the candidates were filed against.

## Decision

**The session document carries the plan's cells, and `Resume` takes them as they stand.**
- **Version 3** of the document writes one `cell` line per node: its id, ring, target orientation
  to 17 significant digits, and acceptance cone. A resumed plan is the begun one bit for bit.
- **A cell is checked as it is read**, since no planner vouches for it: an identity a counter could
  issue, a ring index not below zero, a target that is a rotation, a cone above zero, and one cell
  per id. A version-3 document with no cells is refused, since no plan has none.
- **Version 2 is still read, and replanned** from its spec and lens as every resume was. Refusing
  it would strand every capture in flight when this build arrives, the page's lens among them, whose
  plan the planner lays as it did. A version-2 document carrying `cell` lines is refused: the
  version says what shape the rest is.

The spec is still written, and still returned with the plan. It is what a version-2 document is
replanned from, and the record of what was asked for.

## Consequences

- A capture resumed under a later build is captured against the plan it began with, whatever that
  build's planner lays. `KeepsThePlanItWasBegunWithWhenThePlannerHasSinceChanged` resumes under a
  planner that lays the same ids turned a fifth of a radian, and requires every cell back bit for
  bit.
- Captures begun before this build and resumed after it are replanned, so the window ADR 0076
  recorded stays open for them and closes for every capture begun from now on.
- The document grows by about 76 bytes a cell: 2.4 KB for the page's lens, and about 150 KB at the
  2000-cell bound. It is rewritten after every burst.
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
