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

  The second is a count, per cell, of the picks still in flight: the last of them to **land**
  refreshes. Without it, N quick picks cost 2N reads, and each refresh but the last reads a core a
  later write is about to change.

  Outstanding rather than newest, and the first attempt got that wrong in a way worth recording.
  It counted picks *issued* and refreshed on the newest, which is the same question only while
  answers come back in the order they were asked — an assumption the panel's own comments refuse
  to make, in a comment that claimed to be free of it. Released the other way round, the newest
  write refreshes off a core an older one is about to overwrite, and the older one then stands
  down: the strip keeps a pick the core does not hold until the user leaves the cell and comes
  back. That is this ADR's own failure, reintroduced by the guard added to make it cheaper. Per
  cell for the same reason it is per cell everywhere else here: one counter shared by the panel
  lets a pick in another cell stand down this one's.

  The general shape is worth more than the fix: a guard justified by refusing an assumption has to
  be checked against the case where the assumption fails, and the test fake has to be able to
  produce that case. This one could only ever release writes oldest-first, so every test in the
  file agreed with the assumption the guard was written to reject.

  **And the fix cost something the count it replaced did not.** Counting picks issued was
  *monotone*: nothing ever had to be given back, so no sequence of events could wedge it. Counting
  what is outstanding is a *balance* — the handler acquires before its await and releases after —
  and a cell whose release never runs is a cell that never refreshes again. One `setSelection` that
  never settles leaves that cell's count at one for the life of the page; every later pick on it
  stands down, the core moves, the strip does not, and only re-opening the cell repairs it.

  That case is reachable and it is accepted, because in it the reads hang too: a worker that stops
  answering without firing `error` leaves every call pending, so the refresh this suppresses could
  not have completed either.

  Everything else about the balance is *enforced* rather than warned about, and the first attempt
  at this paragraph got that wrong in a way worth keeping. It said the property had to be written
  down because "no test in the suite would notice, because the fake cannot construct an unbalanced
  count". The first half was true and the second was false — the fake could construct it, with
  affordances added the round before, and the test is twenty-five lines: pick in a cell, leave it,
  let the write land off-cell, come back, pick again, and assert the strip refreshed. Writing a
  warning for the next editor on an unchecked premise about the fake is exactly the move the
  paragraph above this one exists to condemn, one level down again.

  So the release is now in a `finally`, which covers every way out of the block — a rejection, a
  synchronous throw (`ReviewCore` permits one; the generated proxy happens to be `async`, but that
  is a property of generated code rather than of the interface), and an early `return` a later
  edit puts in the middle. And the two cases a test can reach have tests: a pick released while
  its cell was off screen, and a write that throws where a promise was expected.
- **One more round trip per cell opened**, issued alongside the candidate list rather than after it,
  so it costs a `Promise.all` rather than a second wait. Against ~71 µs for a facade call
  (ADR 0019) and the eight preview reads the same open already makes, it is not a number worth
  optimising against.
- **A corrupt selection document fails rather than answering zero.** Saying "nobody chose here"
  about a document that says something unreadable would be inventing an answer.

  And the failure is for the screen, because there is nowhere else for it to go: no `ILogger` is
  wired into anything the core builds, and the panel's `Answered<T>` drops the status by design so
  that `value` cannot be touched on the failing branch. A *rejection* has nowhere at all, so its
  reason goes to the console. Three mechanisms produce one — the worker is dead, it answered
  `failed`, or it answered something that is not a reply — and the second of those is how a
  version skew and a failed allocation arrive, since the facade runs inside the worker and its
  throws are caught there. What they have in common is not that a retry cannot help; a failed
  allocation is transient and a later pick would re-issue the read. It is that none of them is the
  *document's* fault, so a heading blaming the document is wrong about all of them, and the rows
  it leaves clickable are wrong advice for all of them too. So the strip marks *nothing* in force and
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
