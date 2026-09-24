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
worker busy across the reload. Measured in desktop Chromium:

- An idle old worker lets go of both handles before the new worker first asks.
- A busy one is terminated about 2 s after the reload tears its page down, however long it was
  going to be busy for — 3, 8 and 20 s of work all measured the same, 25 of 25 runs, both files
  released in the same instant. CPU throttling at 4x to 20x did not move it.
- The release waits for the reloaded page's main thread to yield: a long task spanning that moment
  pushed it to 4.5 s.

With the old worker busy for 2.4 s or more, the real app refused the resume exactly as the test
did.

`shell/src/access/spill-host.ts` never flushes its sync access handles either, which is the other
way a reload could lose the index; it is not addressed here, because an ordinary reload is not an
OS crash and the page cache keeps what a killed worker wrote.

## Decision

**A reload waits for the resident pair before falling back, and nothing else waits.** The page
reads its own navigation type (`wasReloaded`) and sends it with `boot`, because a worker has none;
the worker picks the wait from it (`handoffFor`).

- **A reload** retries the lock on the frames file, and separately on the index file, each with
  `RELOAD_HANDOFF`: 30 attempts, 100 ms apart, about 2.9 s — the measured release with a second to
  spare. Twenty attempts, the first version of this decision, gave up about 70 ms before Chromium
  let go, and the reload it was written for was refused anyway. Chromium releases the two files
  together, so the index has never needed a second try; a browser that let them go apart could
  make a reload wait up to twice the budget.
- **Anything else** — a second tab, a fresh launch — tries once and falls back, as before this
  decision. The second version of this decision waited in every session, and a second tab opened a
  second before the first was reloaded was then first in line when the reload's old worker let go:
  it took the pair in 5 of 5 runs on a minimal page and 3 of 5 in the real app, and the reloaded
  tab could not resume. With the wait confined to reloads, the same experiment lost the reload's
  pair in 0 of 5.

A name of its own is never waited for, since nobody else can be holding a fresh one, and only the
browser's held-file error (`NoModificationAllowedError`) is waited out: a full disk or a broken
handle will not clear by waiting.

**Every fallback from the resident pair is logged** by the worker with its cause — a held file with
how long it was waited for, anything else by the file it was and the error — and the browser test
that exposed the race prints those logs when its resume is refused, so the next failure says which
tier it got. A browser that cannot lock a file at all still fails at once, since waiting cannot help
it.

**The order in which the worker opens the tier and the documents does not matter**, and nothing
depends on it. A round of review proposed opening the tier first, so the documents would be read
only after the old worker was gone; measured, the two orders read the same snapshot in 49 of 49
reloads. A debounced write the old worker had not yet issued dies with it in either order, and one
it had issued is ordered by IndexedDB itself — a read opened while it is in flight waits for it.

Nor is the page's `pagehide` flush rescued by anything here: on a reload Chromium never delivers
that write at all — 0 of about 50 reloads on a minimal page, and the last pick was lost in 3 of 11
reloads of the real app without the explicit `flush()` the browser test makes. The loss predates
this ADR and is tracked in issue #83.

The wait is a parameter (`ResidentHandoff`), so most tests drive it with a recording sleep rather
than the clock. Two run the reload's budget under fake timers — a pair held for 2.9 s, the measured
release plus the promised second, is waited for, and a pair never released is given up on within
3 s, so the budget is pinned to 30 or 31 attempts — and one shows a session that was not reloaded
does not wait at all.

## Consequences

- A reload whose previous worker is slow to go now gets the resident pair and can resume, instead
  of a tier that makes its own capture unresumable. The reload pays up to 2.9 s of startup when the
  pair is really held by a live second tab. `#enable` starts disabled in the markup until its
  handler is attached, so the wait is a button that cannot be pressed rather than one that does
  nothing when it is.
- A second tab starts exactly as fast as before.
- **A close-and-reopen does not wait.** It is a navigation, not a reload, so a relaunch within about
  two seconds of closing a tab whose worker was busy still falls back. Unmeasured whether that
  happens in life; it is the price of not letting a live sibling jump the queue.
- A reload whose previous worker takes longer than 2.9 s to release still falls back and still
  loses the resume — including one whose own main thread is busy around the two-second mark. The
  budget is measured on desktop Chromium only: nothing here measures how long a torn-down worker
  holds its handles on a phone, or in Safari or Firefox.
- The intermittent browser test is fixed for the mechanism that was reproduced — a busy old worker
  — and not shown fixed for any other. The task tracking it stays open until it stops appearing,
  and a failure now prints the worker's own account of the tier it got.

## Rejected alternatives

**Waiting in every session.** Simpler, and it was this ADR's second version. Rejected because the
waiter that is not a reload has no claim on the pair, yet the one that has waited longest is the
one that gets it when it comes free — measured above.

**The Web Locks API**, holding a named lock for the worker's lifetime and having the next worker
request it. It releases exactly when the old context dies, with no polling. Rejected for now
because it adds a second locking mechanism beside the exclusive handles the files already have,
and those handles are the lock that actually matters: a Web Lock released a moment before the file
handle would still hand the new worker a refusal. Its queue is first-come too, so it would not
settle the second-tab race on its own. Polling the handle asks the question the fallback depends on
directly.
