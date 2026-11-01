# 0085 — A build judges its gaps in a step of its own

**Status:** accepted; the second of ADR 0084's three steps, and amends where that ADR said the gaps
would be reported

## Context

ADR 0084 decided that gaps are found after a build, from where the solve placed each frame, and
built the judgement: `ICoveragePlannerEngine::FindGaps`. Its second step was for
`PanoramaBuildManager` to ask it after `Refine`, through the fitted lens, and for "the build's
progress" to say where the gaps are.

Three things had to be settled to do that:
- Where the answer crosses to the page.
- Which plan the gaps are judged against: a capture that predates stored cells (ADR 0077) carries
  none.
- What a refused judgement does to a build whose panorama does not depend on it.

## Decision

- **`IPanoramaBuildManager::Gaps(BuildId)` answers them, not `BuildProgress`.** A complete build's
  gaps, as `Ghosts` answers a build's movers. `FailedPrecondition` until the build completes, and
  for one that failed.
- **The judgement is a step of its own**, after the solve and before the preview, reported as the
  new stage `CoverageCheck`. So `Poll` still does one thing a call (ADR 0070), and a build has one
  more step than before.
- **What it judges:**
  - Each frame the solve placed, at the solve's rotation, through the solve's lens turned as that
    frame was held (`LensForFrame`, ADR 0082).
  - The fill lens is the solve's lens too. It is the best answer to what the camera sees, and the
    one the fill frames will be taken through.
  - The plan is the cells the capture wrote down, with its spec. A document from before cells were
    written down is replanned from its spec and lens, as `Resume` replans it.
- **A refused judgement does not fail the build.** A planner that cannot replan, or a fill lens
  `FindGaps` refuses, is kept and answered by `Gaps` with the planner's own status, and the build
  completes with its panorama.
- **The page says how many gaps there are, and how much of the sphere.** It asks once the preview
  is drawn, and appends one sentence to the build's line: "Nothing is missing from the sphere.",
  "The sphere has 3 gaps, about 3.2% of it.", or why the gaps could not be judged. Where they are
  is for step 3 to lead the user to.

## Consequences

- **A wrong lens shows only where the solve fitted one.** `Refine` fits the focal length only where
  the capture's loops see it (ADR 0066). Elsewhere the solve's lens is the page's 66-degree guess,
  so gaps from that guess being wrong go unreported, and only gaps from the poses show. A full
  sphere has loops in every band, so this is mainly a cost for partial captures.
- **A replanned capture is judged against today's planner.** Its cells were laid by an older one,
  which is why version 3 writes them down. Judging against the replan is the same compromise
  `Resume` already makes.
- **The answer stays in the core until asked.** A narrow fill lens can mean thousands of targets
  (ADR 0084), and `Poll` is called every step.
- **Every build pays for the judgement.** In a native debug build it takes 48 ms on the default plan
  with a frame at every cell. Its cost on a phone, inside the wasm core, is not measured.
- The page shows a count and a share, not a map. The share comes from `solidAngleFraction`, so a
  gap of seams only too thin to weigh is "under 0.1%" rather than "0%", which would read as none.

## Rejected alternatives

**Carry the gaps in `BuildProgress`**, as ADR 0084 worded it. Every poll would copy them across the
worker, a finished build's on every repeat, and a progress is read for its stage and fraction.

**Fail the build when the judgement is refused.** The preview does not depend on it. A capture whose
cells this build cannot replan would then lose a panorama it can draw, for an answer about
something else.
