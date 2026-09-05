# ADR 0029 — A session is resumed by the manager that owns one, and its frames are adopted back

## Context

Phase 1's exit criterion says a full capture "survives a tab reload and resumes". Nothing in the
core could do it, and two things stood in the way.

The first was where `Resume` lived. `IProjectManager::Resume(ProjectId)` returned a `SessionId`,
and `ProjectManager` has no way to make one: session state — the plan, the per-cell candidate
sets, the pose — belongs to `CaptureSessionManager`, and managers do not call each other
(docs/03 §3.3 rule 3). The method had returned `Unsupported` since it was written, with a comment
saying the client would sequence it once the capture facade landed. That facade has landed, and
the sequencing turned out not to be the problem: there is nothing for a client to sequence.
Whatever reads the document has to be the thing that then holds the session.

The second was the pixels. A capture's frames leave the heap as soon as their cell is committed
(ADR 0023) and end up in the store's spill sink, which on a phone is an OPFS file. The frame store
dies with the tab, and with it every `FrameRef` it ever issued — a fresh store counts from 1
again. A resume that restored only the coverage map would produce the worst possible artefact: a
sphere that says 28 of 28 cells captured, pointing at frames nothing can pin, that builds into
nothing. Restoring the paperwork without the bytes is worse than not resuming at all, because it
looks like it worked.

## Decision

**`Resume` moves to `ICaptureSessionManager`, and is removed from `IProjectManager`.** It reads
the session document, replans, hands the frames it names back to the store, and leaves a live
session behind — the same post-condition `Begin` has.

**`IFrameStoreAccess` gains `Adopt(const FrameRef&)`**: take back a frame this store never
allocated, whose bytes are already in the sink. The identity comes from the caller, which is the
whole point — after a reload the document is the only thing left naming those frames. An adopted
frame arrives `Spilled`, and `Pin` faults it in exactly as it would one the store had demoted
itself. The store steps its own counter past every id it adopts, because a resumed session keeps
capturing from the same counter that issued the restored frames.

`Adopt` is idempotent for a frame the store already holds under the same identity, at the same
size, still in the sink — that is the same frame, not a second one. It matters because a restore
that fails partway leaves frames already taken back, and the only way to undo one is `Forget`,
which takes the sink's copy with it. A resume that tidied up after itself that way would delete
the user's sphere in order to recover from a failure, and every later attempt would pin-fail
against an empty file. So nothing is undone: the document still names those frames, which is the
whole point of it, and the next attempt asks for exactly them.

`Adopt` cannot verify the bytes are down there. `ISpillSink` has no listing, deliberately — the
store is the only thing that knows what it put where — so a frame adopted against a sink that lost
it fails at the `Pin`, carrying the sink's own reason, rather than at the adopt.

**The session document is written at every cell, not only at `End`.** A tab that is evicted
mid-capture never reaches `End`; a document written only there would be exactly the document a
reload cannot find. It is written on the way out of every burst, so what a crash costs is whatever
happened after the last committed cell.

**The plan is replanned from the stored spec and the stored lens, never from the camera in front
of the phone.** Node ids are indices into a particular tessellation. A resume that re-probed the
lens would file every restored candidate under a different cell, silently. The motion capability
is the exception and is read live: the document says which sphere is being captured, never what
the device it came back on can sense.

The document is flat versioned text, parsed all-or-nothing. The generated wire codec belongs to
the boundary (ADR 0013) and a manager reaching into `bridge/` would point a dependency the wrong
way. All-or-nothing because the alternative is a session that comes back missing the cells whose
lines were malformed — a coverage map that quietly lost a cell is worse than one that refuses to
load and says so.

## Consequences

- **A capture survives the tab.** The cells already captured keep counting, and their frames can
  still be pinned, because the store was handed their identities back.
- **`IProjectManager` shrinks.** One fewer method, and no second Resume for a caller to pick
  wrongly between. `List` still reports `nodesSatisfied`, which is what a project picker needs to
  show an unfinished sphere.
- **The candidates win where the document disagrees with itself.** A line carrying more fields
  than this build reads is refused, because taking the prefix that fits is how a session comes
  back missing whatever the extra field was there to say. But the candidate counter is *raised*
  rather than refused when it has fallen behind the ids in the same document — a torn write, with
  the candidate lines newer than the session line. Only the candidates are acted on, so a stale
  counter would have the next burst mint ids naming frames the cell already holds; nothing is lost
  by raising it and a capture is lost by refusing it. The spill index resolves its own
  high-water mark against its slots the same way, for the same reason (ADR 0030).
- **A failed resume leaves the frames it took, and can simply be tried again.** The store outlives
  the attempt — it lives as long as the worker — so a retry meets its own adoptions and accepts
  them. Adopting over a *live* frame is still refused: two different frames under one identity is
  a store that hands the wrong pixels to whoever asks second.
- **The store's identity counter is now something a caller can move.** Adopting an id at or above
  the next one steps the counter past it. Two frames under one identity is a store that hands the
  wrong pixels to whoever asks second, and the ids in a restored document are precisely the ones a
  fresh store would otherwise reissue.
- **`Unsupported` now also means "a document this build cannot read".** The document is kept
  rather than deleted: the bytes it names may still be in the sink, and a build that understands
  its shape may yet come along. Throwing away the only record of a capture is the one
  unrecoverable move available here.
- **The browser half is not done by this.** The spill file is still named per session and swept at
  open (ADR 0020's file-per-session rule), so on a phone the bytes an adopted frame wants are not
  yet where a reload can find them. The core is ready for them; the OPFS index and the page's
  resume flow are the next change, and until then a reload restores the document against an empty
  tier and fails at the first `Pin` — with the sink's reason, which is the honest answer.
- **A cost per committed cell**: one document write, proportional to the candidates captured so
  far. A full sphere's document is a few hundred lines, and it is written once per cell rather
  than once per frame.

## Rejected alternative

**Keep `Resume` on `ProjectManager` and have the client sequence it** — read the document there,
hand it to `CaptureSessionManager.Begin`. This is what the old comment proposed, and it fails on
the shape of the data: the document holds `FrameRef`s and per-cell candidate sets, which would
have to cross the boundary twice, in a contract type invented for the trip, so that a client could
hand a manager back its own private state. The client would also be the thing deciding what a
malformed document means. Resume is not a sequence of two managers' work; it is one manager
reading what it wrote.

**Give the frame store a `Restore()` that lists the sink and takes back everything in it.** It
removes the need for `Adopt` and for the document to carry frames at all. It also requires
`ISpillSink` to grow a listing, which is the seam's one stated invariant — the store knows what it
put where — and it would restore frames belonging to abandoned captures with no way to tell them
from this one's. Identity has to come from the session that owns it.
