# ADR 0040 — A selection is read back from the core, not remembered by the client

## Context

`IProjectManager::SetSelection` records a manual override — the candidate a user picked instead of
the one ranking chose — and marks the node dirty so the next build treats it exactly like a retake
(ADR 0004). It has always been write-only. No contract returns what was recorded.

That left the review panel holding a `Map<NodeId, CandidateId>` of its own writes, with a comment
saying so. Two things follow, and only the second is obvious:

- **A reload forgets the choice.** The strip comes back showing the ranking's own pick in force
  while the build, which reads the document, would use something else. The screen and the output
  disagree, silently, and the screen is the one the user trusts.
- **Even inside one tab, the client is guessing.** Its map is a record of what it *asked for*, not
  of what was *stored*. A write the core refused, or one that failed to reach the store, leaves the
  strip claiming a choice the build will not honour. The panel had a comment about this too — it
  recorded only when the call returned ok — which is a client working hard to stay in sync with a
  fact it could have simply asked for.

This is the second of the two contract-shaped gaps Phase 1's own list named. The first, reading
pixels back out of the store, closed in ADR 0038.

## Decision

**`IProjectManager` gains `GetSelection(ProjectId, NodeId) → Result<CandidateId>`.** The review
client asks on every open and keeps nothing.

**A zero candidate means nobody has chosen here, and it is a success.** `Id::valid()` is
`value != 0` and every counter in these contracts starts at 1, so zero is a value no selection can
have — the same sentinel `Adopt` already refuses a frame for.

It is deliberately not `NotFound`. A caller reads a Result's *status* to tell a call that failed
from one that worked, and the client that needs this most cannot see the status at all: the review
panel's `Answered<T>` narrows to `{ok, value}` and discards the rest, precisely so `value` cannot be
touched on the failing branch. Folding "no override" into the failure branch would make a project
this build cannot read look exactly like one nobody has edited. A project that does not *exist* is
still a failure, because that is a question about the project rather than an answer about the cell.

**The document is parsed, not trusted.** This manager wrote it, but it went through a store that
outlives the process and lives in storage anything with the origin can edit. `from_chars` with the
end pointer checked rejects a partial parse — `"7x"` would otherwise answer 7 to anything that
stopped at the first non-digit, and the pick shown would be a candidate nobody chose.

**And the write side refuses an unset id**, which is what makes the sentinel safe rather than
merely chosen. `SetSelection` used to persist whatever it was handed, so `CandidateId{0}` produced
a document holding zero — a value the reader has to call corrupt, because zero is what it answers
for "nobody has chosen here". A writer able to create state its own reader cannot represent is the
kind of asymmetry a sentinel invites, and the cheapest place to close it is at the door.

**One document per cell, unchanged.** `selection/<node>` is what `SetSelection` already writes, so
setting a pick does not read and rewrite its neighbours.

## Consequences

- **The strip shows what the build will use.** Not a copy of it kept on the client, which was the
  version most able to drift — and the only one whose drift nothing would ever correct. Copies do
  remain further down: `DocumentHost` answers reads from a resident map and commits to IndexedDB
  behind a 250 ms timer (ADR 0014), so the store and the disk disagree for up to that long, and
  for longer if a write fails. That copy is one layer's own business and it converges; the panel's
  did not, because nothing ever told it what had actually been recorded.
- **A refused write needs no undo.** The panel re-opens the cell after recording, and re-opening
  asks the core; a write that never landed leaves the previous answer standing without the client
  having to know it was refused. The branch that used to record only on success is gone, along with
  the map it wrote to.
- **That refresh is gated on two things, and the render ticket is neither of them.** A ticket
  belongs to one render, and two quick picks both carry the render they were drawn in: the first
  write to land refreshes and bumps the ticket, and the second finds its own stale and returns —
  leaving the strip showing the earlier pick while the core holds the later one, which is precisely
  the disagreement this ADR exists to end.

  The first gate is `opened`, the last cell *asked for*. Not the cell painted, which lags it by a
  round trip — and asked-for is the question on purpose: a write landing while the user is already
  on their way elsewhere must not refresh, because a refresh takes the newest ticket and would
  paint the cell they left over the one they are going to. The strip they are walking away from
  does go briefly stale, and it corrects itself when they come back, because coming back reads the
  core rather than a copy.

  The second is a per-cell write count, so only the newest pick refreshes. Without it, N quick
  picks cost 2N reads, and each refresh but the last reads a core a later write is about to change.
  Being a count rather than an assumption about arrival order, it also holds whichever way round
  the two answers come back.
- **One more round trip per cell opened**, issued alongside the candidate list rather than after it,
  so it costs a `Promise.all` rather than a second wait. Against ~71 µs for a facade call
  (ADR 0019) and the eight preview reads the same open already makes, it is not a number worth
  optimising against.
- **A corrupt selection document fails rather than answering zero.** Saying "nobody chose here"
  about a document that says something unreadable would be inventing an answer.

  And the failure is for the screen, because there is nowhere else for it to go: no `ILogger` is
  wired into anything the core builds, and the panel's `Answered<T>` drops the status by design so
  that `value` cannot be touched on the failing branch. So the strip marks *nothing* in force and
  its heading says which one is in force could not be read. It keeps its rows — they were read
  fine, and picking again is how a user repairs the document that failed — but it does not offer
  the ranking's pick as though that were what the build will use. That fold is available and it is
  wrong: it is the screen disagreeing with the build without a word anywhere, which is the thing
  this ADR exists to end, and the client is the last place it could still happen after the core
  stopped doing it.
- **So does a read that failed for any reason other than absence.** Absence is the sentinel and a
  failure is not absence. Every `IProjectStoreAccess` today refuses a missing document with
  `NotFound` and has no other way to fail, but the contract allows one, and folding a storage error
  into "nobody has chosen here" would show the ranking's pick for a cell whose override could not
  be read — without a word anywhere. Only `NotFound` maps to zero.

  That is the third correction of the same kind on this decision, and a fourth followed: a
  sentinel is a value that means something *because* nothing else does, so every place that can
  produce it by accident has to be closed. The writer could produce one (refused now), the reader
  accepted an argument that could only ever produce one (refused now), the reader turned every
  failure into one (narrowed now) — and then the *client* did the fold the core had just stopped
  doing, mapping a failed read onto the same screen as an answer of zero (marked unreadable now).
  None of the four was visible from the decision itself, and the last one is the instructive one:
  closing a fold in the producer does not close it, because the consumer can put it back.
- **Reading a selection cannot enumerate them.** `IProjectStoreAccess` reads a document by key and
  has no listing, so a caller wanting every override in a project must ask cell by cell. Nothing
  needs that today — the strip opens one cell at a time. The build will, and when it does the
  choice is between a listing on the store and one document holding every selection; that is a
  decision to take with a caller in hand rather than in advance.
