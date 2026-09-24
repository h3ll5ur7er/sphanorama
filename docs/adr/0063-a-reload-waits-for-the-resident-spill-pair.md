# 0063 — A reload waits for the resident spill pair before taking a tier of its own

**Status:** accepted

## Context

ADR 0030 gave the spill tier a fixed preferred name, so a reload can find the frames its session
document names, and kept ADR 0020's fallback: a session that cannot lock the resident pair takes a
tier under a name of its own. It named two sessions that end up there — a second tab open on the
app, and a reload whose previous worker has not been torn down yet — and treated them alike.

They are not alike. A second tab holds the pair for as long as it lives, so falling back is the
only way it captures at all. A reload's previous worker is on its way out and lets go within
moments. Falling back at the first refusal hands the reloaded page a tier with a fresh generation,
so `Resume` compares the document's generation against the wrong tier and refuses the capture as
"captured into a spill tier this device no longer holds" — the loss the resident pair exists to
prevent, in the case it was written for.

The roadmap's Phase 1 entry named this race as open. The browser test "a pick survives the tab
that made it" failed intermittently with exactly that refusal, in three of ten full local runs on
2026-09-23 and 2026-09-24, and in no CI run. Instrumented runs afterwards did not reproduce
it — thirteen runs, four under heavy CPU and disk load. What did reproduce it was keeping the old
worker busy across the reload: Chromium lets go of an idle worker's handles before the new worker
first asks, and terminates a busy one about 1.98 s after the new worker starts polling, whatever
it was busy with (3, 8 and 20 s of work all measured the same, 25 of 25 runs, both files released
in the same instant). With the old worker busy for 2.4 s or more, the real app refused the resume
exactly as the test did. `shell/src/access/spill-host.ts`
never flushes its sync access handles either, which is the other way a reload could lose the
index; it is not addressed here, because an ordinary reload is not an OS crash and the page cache
keeps what a killed worker wrote.

## Decision

**`openSpillTier` waits for the resident pair before falling back.** It retries the lock on the
frames file, and separately on the index file. Each file gets `RELOAD_HANDOFF`: 30 attempts,
100 ms apart, about 2.9 s — the measured release with a second to spare. Twenty attempts, the
first version of this decision, gave up about 70 ms before Chromium let go, and the reload it was
written for was refused anyway. Chromium releases the two files together, so the index has never
needed a second try; a browser that let them go apart could make a reload wait up to twice the
budget. Only then does it take a name of its own, exactly as before. A name of its own is never
waited for, since nobody else can be holding a fresh one, and only the browser's held-file error
(`NoModificationAllowedError`) is waited out: a full disk or a broken handle will not clear by
waiting.

**The worker opens the tier before it reads the session documents.** The previous worker releases
the resident pair only when it is gone, so holding the pair means the snapshot is taken after it
can commit nothing more, rather than racing a debounced write it had in flight. The order lives in
`shell/src/bridge/stores.ts` rather than inline in the worker, so a test can hold the tier open and
see that no document is read.

That is all it buys. It does **not** rescue the page's `pagehide` flush: on a reload Chromium never
delivers that write at all — 0 of about 50 reloads on a minimal page, and the last pick was lost in
3 of 11 reloads of the real app without the explicit `flush()` the browser test makes. The loss
predates this ADR and is tracked in issue #83; this decision neither causes it nor closes it.

**Every fallback from the resident pair is logged** by the worker with its cause — a held file with how long it was waited for, anything else by its error name — and the browser test that exposed the race
prints those logs when its resume is refused, so the next failure says which tier it got. A browser that cannot lock a file at
all still fails at once, since waiting cannot help it.

The wait is a parameter (`ResidentHandoff`), so most tests drive it with a recording sleep rather
than the clock. Two run the shipped default under fake timers — a pair held for the measured 2 s
is waited for, a pair never released is given up on within 3.5 s — because the worker calls
`openSpillTier()` without one and those are the values that decide whether a reload resumes and
whether a second tab boots.

## Consequences

- A reload whose previous worker is slow to go now gets the resident pair and can resume, instead
  of a tier that makes its own capture unresumable.
- **A second tab now takes about three seconds longer to start**, because it waits out the whole
  handoff before falling back. That is the tab that cannot resume anyway, and it still gets a tier.
  `#enable` starts disabled in the markup until its handler is attached, so the wait is a button
  that cannot be pressed rather than one that does nothing when it is.
- A reload whose previous worker takes longer than 2.9 s to release still falls back and still
  loses the resume. The budget is measured on desktop Chromium only: nothing here measures how long
  a torn-down worker holds its handles on a phone, or in Safari or Firefox.
- The intermittent browser test is fixed for the mechanism that was reproduced — a busy old worker
  — and not shown fixed for any other. The task tracking it stays open until it stops appearing,
  and a failure now prints the worker's own account of the tier it got.

## Rejected alternative

**The Web Locks API**, holding a named lock for the worker's lifetime and having the next worker
request it. It releases exactly when the old context dies, with no polling. Rejected for now
because it adds a second locking mechanism beside the exclusive handles the files already have,
and those handles are the lock that actually matters: a Web Lock released a moment before the file
handle would still hand the new worker a refusal. Polling the handle asks the question the fallback
depends on directly.
