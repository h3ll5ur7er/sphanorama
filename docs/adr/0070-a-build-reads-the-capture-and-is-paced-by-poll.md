# 0070 — A build reads the capture's document, and is paced one step per `Poll`

**Status:** accepted

## Context

Every engine a first panorama needs now exists: `ExtractFeatures`, `EstimatePairwise` and `Refine`
register a ring to hundredths of a degree (ADR 0065, ADR 0066), and `RenderPreview` composes one
(ADR 0068). Nothing calls them outside tests. `PanoramaBuildManager` answered `Unsupported` to
everything, and it could not have done otherwise as declared, for three reasons.

- **`Start` took a `SessionId`, which names nothing the build manager can reach.** A session id is
  a counter `CaptureSessionManager` issues and keeps; the only thing that could turn one into frames
  is that manager, and managers do not call managers. What the build needs — which cells hold which
  frames, the pose each was taken at, the lens — is already written down, in the session document
  every burst checkpoints to the project's store (ADR 0029). A document is addressed by project.
- **That document had one reader, private to its writer.** The codec lived in
  `capture_session_manager.cpp`. A second copy in the build manager is two formats the day one
  changes, and the version gate that refuses a document from another shape would be enforced in one
  of them.
- **A build is long, and the core worker has one thread.** Twelve frames extract in 0.16 s (ORB) to
  0.94 s (SIFT) under node on a desktop (ADR 0069); a phone is slower and a sphere is sixty frames.
  A `Start` that ran all of it would hold the worker for tens of seconds, during which nothing — not
  a `Poll`, not a `Cancel` — could be answered, and there would be no progress to report because
  nothing could ask for it. The pthread pool may be zero (docs/04 §4.1), so a background thread is
  not something to rely on.

## Decision

1. **`Start` takes a `ProjectId`.** A contract change: the build reads the project's session
   document, exactly as `Resume` does, and `IProjectManager::Export` already names a project beside
   a build. The TypeScript mirror and the facade move with it.
2. **The session document's codec is a utility**, `utilities/session_document`, with the key a
   cell's manual pick is recorded under and the parser for it. A pure move; it still has one writer,
   `CaptureSessionManager`. `ProjectManager` still only asks whether a document exists.
3. **`Start` does the cheap part and `Poll` does the rest, one step a call.** `Start` reads and
   checks everything it can without reading a pixel, and queues the steps: one per frame's
   features, one per pair, one solve, one preview. Each `Poll` performs the next and reports where
   the build is, so progress is real, a `Cancel` between two steps is answered at once, and the
   longest the worker is held is one step — a frame's extraction, one pair, the solve or the
   preview — rather than all of them. This is ADR 0018's shape — a burst paced
   across the ticks the client already makes — applied to the other long thing the core does.
4. **What a build is made from.**
   - **One frame per cell**: the cell's manual pick where `ProjectManager` recorded one, and
     otherwise its best-ranked candidate in the document whose pose was measured (see below) —
     which amends ADR 0026's automatic pick of `Candidates(node)[0]` — the document lists each cell's candidates in the order the ranking left
     them, and `CaptureSessionManager` now rewrites it after a discarding retake and after an offer
     as well as after every burst, so it does not name frames the store has forgotten. A pick the
     cell no longer holds gives way to the ranking: a discarding retake leaves the pick behind with
     no way to clear it, and no screen can show a frame that is gone. The first version refused
     such a pick, which made every build after a retake of a picked cell impossible. A pick at or
     past the candidate counter the document recorded is refused instead: the document never saw
     it, because a rewrite failed, and the ranking would build from a frame other than the one the
     user chose. A pick a retake discarded is always below that counter.
   - **A pick always names the frame it was made on.** `CaptureSessionManager` issues no candidate
     identity a recorded pick names, at `Begin` or at `Resume`, and writes the stepped counter down
     at once: the document's counter alone would reissue the one a pick made after a failed rewrite
     names, once the tab reloads, and a new tab's counter restarts at 1 while an earlier capture's
     picks survive. Either would hand the pick to a frame nobody chose, and the build would honour
     it. A pick the store cannot read refuses both.
   - **Identities have two bounds.** A reader accepts any below 2^53, since an identity crosses to
     the page and the spill sink as a double and one past that arrives as its neighbour's. But a
     counter stepped past a stored identity — a pick, or a document's session, candidate or frame on
     `Resume` — needs the identity well below that, or the counter issues past what the reader
     accepts within a burst or two. So a pick, and every identity `Resume` steps past, must be below
     2^52: half the range left above, which no capture comes near issuing. `SetSelection` refuses a
     pick past it, and `Resume` refuses a document naming one, rather than stepping a counter there.
     Bounding the reader alone at the limit was tried twice in review and each time moved the edge:
     a counter stepped to just below it issued past it. The cost is that a frame issued at or past
     2^52 — which only a pick or document edited to near the bound can lead to — cannot be picked.
   - **Only a measured pose is read.** A frame whose pose has confidence zero is paired with
     nothing, because at zero the orientation is not a measurement (`PoseSampleDefect`) and the
     direction a degenerate one normalises to is straight ahead. A capture where no pose was
     measured is refused at `Start`, as is one naming a frame in two cells: the solve refuses both,
     and would say so only after every extraction and pair had been paid for.
   - **The lens is the capture's field of view at the grabbed frames' size**
     (`LensFromFieldOfView`), not the camera's maximum size the document records: the page grabs at
     most 1280 on the long edge, and matches are pixels of the grabbed frame. Every selected frame
     must be that one size.
   - **Pairs are cells whose frames look within the lens's narrower field of view of each other**,
     measured between the directions the capture's poses say they looked. On a ring that is the
     neighbours and the closing pair, which is the edge a chain throws away (ADR 0065). A pair whose
     frames did not register is left out, as a declined edge; an unaccepted one is kept, since
     `Refine` ignores it by contract (ADR 0056). A frame with no features is paired with nothing and
     placed by its prior.
   - **Every frame is put back in the tier it was found in** after its features are read, and after
     a preview that refused having left one pinned or faulted in (`RenderPreview` says it may),
     since
     extraction leaves a spilled frame resident (`FeatureSet`'s comment), and a sphere's worth of
     faulted-in frames is the heap refusal ADR 0023 exists to avoid. What the store will not put
     back is retried by `Cancel` and the next `Start` — and by then a frame may have been cooled by
     the capture or pinned by somebody `Pin` made a promise to, so putting back only ever cools a
     frame, and releases the one pin an engine left exactly once. A preview whose frames' tiers
     cannot be read is not drawn, since a refusal could not then be put right. The feature sets are
     forgotten once the last pair is estimated: `Refine` reads the pairs' matches, not the sets; a
     pin an engine left on one is the build's to release, since nothing else was given its handle.
5. **What a build answers.** A finished build's panorama is the preview: an equirectangular frame
   no wider than `BuildSpec::outputWidth` or 2048, held by the core until the build is cancelled or
   another is started. No tiles, no ghosts, no incremental rebuild — `Ghosts` and `Invalidate`
   answer `Unsupported` for a build that exists, rather than an empty report that reads as "no
   movers" or a success that rebuilt nothing.
6. **One build at a time.** `Start` refuses while one is running; starting another after one has
   finished releases the finished one's panorama.

## Consequences

- **A host with no spill tier captures but does not build.** That is a browser with no origin
  private file system, one whose handle would not open (`bridge/runtime.h`), and every native
  build. Its tier generation is zero in every tab and frame identities restart with each, so a
  document an earlier tab wrote names ids a live capture now holds, and the store answers for
  them. Nothing the build can ask tells the two apart — `Resume` is refused on such a host by
  `Adopt` for any document that names a frame — so `Start` refuses rather than build from another
  capture's pixels. This is a real cost: such a browser was "degraded rather than broken" for
  capture and is now broken for the panorama. A store that could say which tab it belongs to is
  what would lift it, and `TierGeneration`'s contract now says zero cannot be matched by id.
- **ADR 0065's "placed through its pairs" does not hold in a build.** A frame whose pose was not
  measured — one `Resume` demoted, or a pick of one — is paired with nothing, because the build
  pairs by the direction a pose gives, and `Refine` drops it. So a cell ranked without a manual
  pick gives its best *measured* frame, and a cell whose only or picked frame is unmeasured is
  left out of a panorama that still completes. Surfacing `droppedFrames` to a client is a
  contract change for the change that shows a build.
- **An offered frame is never built from**, even when it ranks first or is picked, because the
  document leaves it out — its bytes are not in the tier (ADR 0023). Nothing offers a frame today.
- **The review strip keeps a copy of the rule.** Which frame a cell gives where nobody picked —
  the best measured, else the best — is decided in `Start` and marked by
  `shell/src/clients/review/candidates.ts`, which reads the same candidates. A second copy rather
  than a question to the core, because asking would be a contract change for one boolean; the two
  name each other and move together.

- **A contract change** in `managers/panorama_build_manager.h`, and the generated TypeScript and
  dispatch with it. Nothing in the shell calls `Start` yet.
- **The browser's build still fails at its first step.** Its composition root holds
  `NullRegistrationEngine` and `NullCompositionEngine`, so the first `Poll` reports `Failed` with the
  engine's `Unsupported`. Selecting the real ones is the change that moves 556 KB into the module
  (ADR 0069) and takes a detector, a feature cap and a preview width sized for a phone, and it gets
  its own change with the page that starts a build and shows it.
- **The kept lens still has no writer** (ADR 0067). Its key is the camera's `deviceId` and the frame
  shape, and the session document carries no `deviceId`, so a build cannot say which device's lens
  to amend. `Refine` is handed the capture's field of view as a guess, which any precise fit
  replaces, so a ring builds under its fitted lens all the same; what is lost is the next capture's
  head start.
- **A capture that no longer has its pixels is refused at `Start`**, by asking the store where each
  selected frame is: a tab reloaded without `Resume`, or a tier emptied by a newer capture, which
  the document's tier generation also catches (ADR 0035). Refused before any step runs, rather than
  failing at the first `Pin` with a store's `NotFound` and a build half made.
- **Feature sets are held across `Poll` calls.** Up to one per frame, until the pairs are done — a
  sphere's worth of descriptors at the engine's feature cap. That is a memory figure to measure on a
  phone with the change that selects a detector, not one this change can state.
- **Progress counts steps, not time.** An extraction and a pair are one step each though they cost
  different amounts; `fraction` is monotone and reaches one exactly when the build does, which is
  what a progress bar needs and all it promises.

## Rejected alternative

**The client hands `Start` the frames.** The page would read each cell's candidates from
`CaptureSessionManager`, its picks from `ProjectManager`, and pass the chosen frames and poses
across — the layer rules allow a client to sequence two managers. Rejected because it makes the
client the owner of which frame represents a cell, and that decision is the one the review screen
and the build must agree on: a client that got it wrong would build a panorama from frames the
screen did not show, and nothing in the core could notice. It would also put a sphere's poses and
handles on the wire in both directions for data the core already holds in a document it wrote.
