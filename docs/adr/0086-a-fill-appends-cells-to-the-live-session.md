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

- **`ICaptureSessionManager::FillGaps(found)` appends to the live session's plan**, writes it
  down, and answers the plan that results. There is no resume, because the session never stopped.
  `found` is `IPanoramaBuildManager::Gaps`' answer whole: the gaps, and the capture they were
  judged against — the project, and the session and candidate counter of the document `Start`
  read, a `CaptureRevision`. The project is there because sessions are counted per core, so
  another tab's capture of another project can share a session number.
  - `FailedPrecondition` with no session, and while a burst is armed or firing. The page holds its
    calls through a burst anyway (ADR 0071).
  - `FailedPrecondition`, too, while any cell of the plan is still to shoot, fill cells included.
    A build of a capture with unshot cells finds their directions open, and the guide already
    leads there: a fill would put a second cell beside each, and one pressed after they were shot
    would lead back to directions already taken. Counting fill cells is what stops a second
    press from filling the same gaps twice. A cell counts as shot only by a frame the document
    holds: an offered frame is never written down, so a cell it alone covers is open to every
    build, and its gap would be filled on top of it.
  - `FailedPrecondition`, too, for gaps judged against a capture other than the session as it
    stands: another project's or session's, or this one before a frame it now has. Every frame steps
    the counter, so equal revisions have taken no frame since; a replace-retake lets frames go
    without stepping it, and a cell it empties is unshot. A cell shot since the build read the
    document — during the build, or under a checkpoint the store refused — is open in its gaps, and
    filling it would put a cell beside itself; the same stale gaps pressed twice are refused twice.
    The capture is written down before this refusal, since a capture with every cell shot fires no
    burst to checkpoint it and every later build would read the same stale document; it is the one
    refusal that changes the document. A pick is not part of the revision: switching one changes
    which of a shot cell's frames a build places — by up to twice the cell's cone, by the poses the
    burst recorded, since each frame is held to the target and not to the others — and never whether
    the cell is shot, which is what a fill guards.
  - A precondition is asked before the argument is, so an empty fill is refused while the capture
    could not take one, rather than answered with the plan.
- **The client sequences the two managers.** The build client already holds the gaps. It offers a
  button, "Add the gaps to the plan", whose press hands them to `FillGaps`. The loop then takes the
  grown plan, and the map and markers draw the new cells once coverage of it has been read.
  - The offer is made only while the capture could act on it: its loop is running, so there is a
    guide to lead to the new cells, and every planned cell is shot. It is asked as the build
    starts as well as when its gaps come back: the gaps are judged against the capture the build
    read, and a cell shot while it ran is one of them. It is asked again at the press, and after
    the fill answers, which says to reload when the guide has gone in the meantime.
- **A new cell:**
  - is aimed at its target, which `FindGaps` already answers level;
  - takes the capture's cone;
  - takes an identity past every cell the plan has;
  - sits on a ring of its own, past the plan's highest.

  Nothing existing moves, so every candidate stays under the cell it was filed under. A plan with
  fill cells is one `Resume` takes as stored, and one a build judges its gaps against.
  - A new identity can be one an earlier capture of the same project used, with a pick recorded
    under it. The candidate counter steps past every pick of the grown plan, as `Begin` and
    `Resume` step past theirs (ADR 0070), so no pick names a frame this capture issues.
  - A plan whose identities or rings are at the top of their range is refused, rather than
    stepped past it.
- **Bounded by one number:** `kMostCellsAPlanHolds`, 2000. It is the planner's existing limit, now
  in `utilities/session_document`, and `PlanDefect` refuses a plan over it. So the planner, the
  decoder, `Begin` and a fill all hold one bound. A fill that would pass it is refused whole, since
  part of a gap is not a sphere any closer to complete.
- **Held to `PlanDefect` before it is kept, and written down before it is answered.** A refusal
  leaves the plan and the counter as they were, and the document too but for the one above, and
  what is written down is what a resume can read. A store that will not write the plan down
  refuses the fill with its own status, rather than leaving fill cells only the tab knows of. A
  target that is not a rotation is refused explicitly, because `Normalize` would answer the
  identity for it.

## Consequences

- **The first `uint64_t` on the wire that is not a content hash.** `CaptureRevision::nextCandidate`
  is a counter, and it crosses as eight raw bytes, a `bigint` on the page, while the identities the
  same counter issues cross as branded doubles. It crosses that way because the document's field is
  a `uint64_t`, kept exact so that an equality needs no argument about range — not because a live
  counter could lose a bit, since one stays below 2^53. ADR 0013's encoding rule is amended to say
  so.
- **The loop's plan can change under it.** The page's render loop held its plan as a constant. It
  now takes a grown plan in place: the cones and the count are read from the plan rather than
  copied beside it, and the coverage map is redrawn. Nothing else in the loop holds a cell by
  position. No test drives the page through an accepted fill, since the end-to-end suite would have
  to shoot every cell of a plan first; the core's fill and the client that hands its plan on are
  tested, and the loop's taking it is not.
- **The guidance leads to fill cells as to any other.** They have no candidates, so `Locate` names
  the nearest as needed and the dwell fires on it. A fill cell aimed between two shot cells may be
  reached inside another cell's cone first, which is aim-first targeting as ADR 0041 decided.
- **A fill is a step at the end of a capture, not during it.** It waits until every planned cell
  is shot, so a capture's gaps are filled once its plan is done, and filled again only after the
  fill cells are shot and a new build finds what they left. A gap a build finds while cells are
  still to shoot is mostly those cells, and the guide is already leading to them.
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

**Ask the stored document at fill time whether it files every shot cell**, rather than carry the
capture a build judged. It needs no contract change, and it was the first fix for a checkpoint the
store refused. But it asks the document as it is *now*, and the gaps describe the document the
build read: the catch-up write that refusal makes is the same write that lets the same stale gaps
through on a second press, and a build started before the last cell was shot reads a document the
burst has since brought up to date. It also read a store's refused read as a document behind, and
it could not catch up a cell an offered frame alone covers, which the document never files. A
revision is compared rather than read, so the first cannot arise; the second is why a fill counts
as shot only what the document holds.
