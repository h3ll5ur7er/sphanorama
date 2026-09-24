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
it — thirteen runs, four under heavy CPU and disk load — so this decision rests on the mechanism
the code's own comment describes, not on a captured failure. `shell/src/access/spill-host.ts`
never flushes its sync access handles either, which is the other way a reload could lose the
index; it is not addressed here, because an ordinary reload is not an OS crash and the page cache
keeps what a killed worker wrote.

## Decision

**`openSpillTier` waits for the resident pair before falling back.** It retries the lock on the
frames file, and separately on the index file, since the old worker releases its two handles
independently. Each file gets `RELOAD_HANDOFF`: 20 attempts, 100 ms apart, about two seconds, so
a reload whose old worker lets go of the frames late and the index later still can wait close to
four. Only then does it take a name of its own, exactly as before. A name of its own is never
waited for, since nobody else can be holding a fresh one. A browser that cannot lock a file at
all still fails at once, since waiting cannot help it.

The wait is a parameter (`ResidentHandoff`), so most tests drive it with a recording sleep rather
than the clock. One runs the shipped default under fake timers, because the worker calls
`openSpillTier()` without one and those are the values that decide whether a reload resumes.

## Consequences

- A reload whose previous worker is slow to go now gets the resident pair and can resume, instead
  of a tier that makes its own capture unresumable.
- **A second tab now takes about two seconds longer to start**, because it waits out the whole
  handoff before falling back. That is the tab that cannot resume anyway, and it still gets a tier.
- A reload whose previous worker takes longer than two seconds to release still falls back and
  still loses the resume. The budget is a judgement, not a measurement: nothing here measures how
  long a torn-down worker holds its handles on a phone.
- The intermittent browser test is not shown to be fixed, only its known mechanism. The task
  tracking it stays open until a failure is captured with the cause visible, or it stops
  appearing.

## Rejected alternative

**The Web Locks API**, holding a named lock for the worker's lifetime and having the next worker
request it. It releases exactly when the old context dies, with no polling. Rejected for now
because it adds a second locking mechanism beside the exclusive handles the files already have,
and those handles are the lock that actually matters: a Web Lock released a moment before the file
handle would still hand the new worker a refusal. Polling the handle asks the question the fallback
depends on directly.
