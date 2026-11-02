# 0086 — A fill appends cells to the live session's plan

**Status:** accepted; the third of ADR 0084's three steps, and amends where that ADR said a fill
resumes; extends [0077](0077-a-capture-keeps-the-plan-it-was-begun-with.md), whose plan is kept as
begun and now grows

## Context

ADR 0084 decided that the gaps a build finds are filled by new cells: "`CaptureSessionManager`
appends a cell at each target to the stored plan and resumes, so the reticle leads there". ADR 0085
built the second step, so a build now answers `IPanoramaBuildManager::Gaps`.

The page never ends a capture session: it runs for the life of the tab, a build is started under it,
and a reload resumes it (ADR 0029). So by the time a build has found gaps, a session is always live.
Resuming one would mean ending the session that is running only to read its own document back.

Three more things had to be settled:
- How the gaps reach the capture, when managers never call each other.
- What a new cell is: its identity, ring, aim and cone.
- What bounds a fill. A narrow fill lens can ask for thousands of targets (ADR 0084).

## Decision

- **`ICaptureSessionManager::FillGaps(gaps)` appends to the live session's plan**, writes it down,
  and answers the plan that results. There is no resume, because the session never stopped.
  - `FailedPrecondition` with no session, and while a burst is armed or firing. The page holds its
    calls through a burst anyway (ADR 0071).
- **The client sequences the two managers.** The build client already holds the gaps. It offers a
  button, "Add the gaps to the plan", whose press hands them to `FillGaps`. The loop then takes the
  grown plan, and the map and markers draw the new cells.
- **A new cell:**
  - is aimed at its target, which `FindGaps` already answers level;
  - takes the capture's cone;
  - takes an identity past every cell the plan has;
  - sits on a ring of its own, past the plan's highest.

  Nothing existing moves, so every candidate stays under the cell it was filed under. A plan with
  fill cells is one `Resume` takes as stored, and one a build judges its gaps against.
- **Bounded by one number:** `kMostCellsAPlanHolds`, 2000. It is the planner's existing limit, now
  in `utilities/session_document`, and `PlanDefect` refuses a plan over it. So the planner, the
  decoder, `Begin` and a fill all hold one bound. A fill that would pass it is refused whole, since
  part of a gap is not a sphere any closer to complete.
- **Held to `PlanDefect` before it is kept.** A refusal leaves the plan and the document as they
  were, and what is written down is what a resume can read. A target that is not a rotation is
  refused explicitly, because `Normalize` would answer the identity for it.

## Consequences

- **The loop's plan can change under it.** The page's render loop held its plan as a constant. It
  now takes a grown plan in place: the cones and the count are re-derived, and the coverage map is
  redrawn. Nothing else in the loop holds a cell by position.
- **The guidance leads to fill cells as to any other.** They have no candidates, so `Locate` names
  the nearest as needed and the dwell fires on it. A fill cell aimed between two shot cells may be
  reached inside another cell's cone first, which is aim-first targeting as ADR 0041 decided.
- **Filling twice appends twice.** A second build finds whatever the first fill's frames left open,
  and its fill adds cells for that. Nothing deduplicates against cells already added and not yet
  shot. A press per build is the expected rhythm, and the page withdraws the offer once used.
- **A narrow fill lens is refused rather than truncated.** With nothing placed, a 15.5-degree fill
  lens asks for 9,342 targets (ADR 0084), far past the bound. The page says why. A partial fill
  would need a rule for which gaps come first, and nothing decides that yet.
- The stage line still says how many cells were planned when the capture began.

## Rejected alternatives

**End the session and resume it with the fill cells in the document**, as ADR 0084 worded it. It
reuses `Resume`'s checks. But it closes the camera and re-adopts every frame to get back exactly the
session that was running, and a resume refused at that point would leave the user with no session
at all.

**Have `Begin`'s planner replan with the fitted lens.** It would lay a tighter plan, but under new
identities, and every candidate would be filed under the wrong cell. ADR 0077 exists to prevent
exactly that.
