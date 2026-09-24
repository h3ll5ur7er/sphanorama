# 0063 — The resident spill pair is handed to the page that replaces its holder

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

Polling the file cannot decide who should have the pair next: whoever asks first after it comes
free gets it. Four versions of this decision chose who may poll, each was measured handing the pair
to the wrong session, and each is recorded under the rejected alternatives with the number that
retired it.

`shell/src/access/spill-host.ts` never flushes its sync access handles either, which is the other
way a reload could lose the index; it is not addressed here, because an ordinary reload is not an
OS crash and the page cache keeps what a killed worker wrote.

## Decision

**The right to the resident pair is a Web Lock, held by a page for its whole life, and handed to
the page that replaces its holder.** The files stay the lock that matters to the worker; the Web
Lock decides who may ask for them. Page side it is `bridge/tier-claim.ts`; the worker is told the
outcome with `boot` and never touches the pair without it.

- **The holder's successor queues for the right.** A lock is granted to the first waiter the moment
  its holder goes, so the successor is next without having to be quick. Its worker then polls the
  files with `RELOAD_HANDOFF` — 30 attempts, 100 ms apart, about 2.9 s, the measured release with a
  second to spare — because a busy old worker lets go of them up to about two seconds after its
  page. Nobody else can take them in that gap. (`access: 'wait'`)
- **Everyone else asks only if the right is free** (`ifAvailable`), so it can never jump the queue,
  and tries the files once if it gets it (`'try'`). If another page holds the right, the worker
  leaves the pair alone entirely and takes a tier of its own (`'skip'`).
- **A page whose worker did not get the resident pair gives the right up**, and so does one whose
  worker failed to boot or died, so the page the pair belongs to can be handed it.
- **A page lets the right go on `pagehide`**, just after stamping its departure, rather than when it
  dies: Chromium never puts a page holding a Web Lock in the back/forward cache (measured: `main`
  restored on Back 3 of 3, a page holding the lock for life 0 of 3, reason `WebLocks`). The stamp
  keeps newcomers off until the successor has queued, and a cached page's frozen worker still holds
  the files, so a newcomer that takes the right meanwhile fails its one try and gives it back. A
  page restored from the cache takes the right back on `pageshow`.

**Who is the successor is decided from two records.** On `pagehide`, a page holding the right stamps
its departure — its token and the time — in its tab's `sessionStorage` and in the origin's
`localStorage`, which names the page holding the right. A page is the successor if its tab has a
departure from the last `FRESH_MS` (5 s) and that departure names the holder the origin still has.
So the successor is the next page in the tab, however it arrived — a reload, the same URL again,
Back — and not a new tab (no departure), a duplicated one (the departure is read and removed at
startup, so a live page has none to copy), or a tab back at the app after someone else took over
(the origin names someone else). Where the origin's record cannot be read, the tab's alone decides.

**A newcomer stands aside for `HANDOVER_MS` (2 s) after any departure**, because between the old
page going and its successor queueing, the right is briefly free and nobody has asked for it yet.

A name of its own is never waited for, since nobody else can be holding a fresh one, and only the
browser's held-file error (`NoModificationAllowedError`) is waited out: a full disk or a broken
handle will not clear by waiting.

**Every fallback from the resident pair is logged** — by the worker, with the file and the error,
and by the page when a successor gives up waiting for the right — and the browser test that exposed
the race prints those logs when its resume is refused, so the next failure says which tier it got.

**Measured against this design**, in the real app, desktop Chromium, old worker busy for 8 s where
the case needs one. Playwright disables the back/forward cache by default, so the rows below other
than the cache's own ran without it, which is the path a page takes when the cache does not keep
it:

| Case | Result |
| --- | --- |
| Reload, same URL, Back — the successor over a busy old worker | got the pair 3 of 3 each, ready in about 2.35 s |
| A new tab opened 1.925 to 1.975 s after a busy reload, when the old worker is killed | reload kept the pair 7 of 7 |
| A new tab opened at the instant of an idle reload, or 20 ms after | reload kept the pair 15 of 15 |
| A duplicated tab, then its original reloaded | original kept the pair 4 of 4 |
| A second tab reloaded, then the first | first kept the pair 4 of 4 |
| A tab back at the app after another took over, then that one reloaded | no wait (about 120 ms); the other kept its pair 5 of 5 |
| Back to the app with the cache enabled | restored from the cache 3 of 3, holding the right again |
| Back after more than 2 s away, with a newcomer opened meanwhile | Chromium evicted the cached page and the newcomer got the pair |

**The order in which the worker opens the tier and the documents does not matter**, and nothing
depends on it. A round of review proposed opening the tier first, so the documents would be read
only after the old worker was gone; measured, the two orders read the same snapshot in 49 of 49
reloads. A debounced write the old worker had not yet issued dies with it in either order, and one
it had issued is ordered by IndexedDB itself — a read opened while it is in flight waits for it.

Nor is the page's `pagehide` flush rescued by anything here: on a reload Chromium never delivers
that write at all — 0 of about 50 reloads on a minimal page, and the last pick was lost in 3 of 11
reloads of the real app without the explicit `flush()` the browser test makes. The loss predates
this ADR and is tracked in issue #83. The departure stamps are synchronous `sessionStorage` and
`localStorage` writes, and those do land: a successor is recognised only when both are there and
agree, and every successor in the table above was.

The tests drive the page's decision against a fake lock manager with the queueing, `ifAvailable`
and abort behaviour the design leans on, one test per case above; the worker's wait against a
recording sleep and, for the shipped budget, fake timers pinning it to 30 or 31 attempts at 100 ms;
and a browser test drives the chain end to end — a second tab and its reload leave the pair alone,
a forged departure is no claim, and the first tab's reload is handed the pair.

## Consequences

- A page that replaces the holder in the same tab gets the resident pair and can resume, however
  it came back and however busy the old worker was.
- A second tab starts as fast as before, and cannot take the pair from anyone.
- **A tab opened within two seconds of the app's last tab closing gets a tier of its own**, because
  it cannot tell a close from a reload whose successor has not queued yet. Its capture is then not
  resumable from there. This was already so within about two seconds on the old worker's account.
- A successor whose old worker takes longer than 2.9 s to let go of the files still falls back —
  including one whose own main thread is busy around the two-second mark. Measured on desktop
  Chromium only: nothing here measures a phone, Safari or Firefox.
- **Without Web Locks the files alone decide**: a successor polls them and everyone else tries once,
  this ADR's weakest earlier version. It is reachable almost nowhere — the SIMD core already needs
  Safari 16.4, well after Web Locks, and an insecure context has no origin private file system to
  spill to — but deciding the right never stops the page loading: anything that throws on the way,
  such as a missing `crypto.randomUUID` or `AbortSignal.timeout`, falls back the same way.
- **A page away in the back/forward cache for more than two seconds can lose its pair** to a
  newcomer, which Chromium lets have the files by evicting the cached page. The tab then comes
  back as a fresh page with a tier of its own.
- The right assumes one app document per tab. Two same-origin frames of the app in one tab share a
  `sessionStorage`; nothing ships that embeds the app.
- The intermittent browser test is fixed for the mechanism that was reproduced — a busy old worker
  — and not shown fixed for any other. The task tracking it stays open until it stops appearing.

## Rejected alternatives

Each was a version of this decision, and each is here with the measurement that retired it.

**Fall back at the first refusal** (before this ADR). A reload over a busy old worker lost its
resume.

**Every session polls the files.** A second tab opened a second before the first was reloaded was
first in line when the old worker let go: 3 of 5 in the real app, 5 of 5 on a minimal page.

**Every reload polls** (the navigation type). Same-URL navigation leaves a worker behind just as a
reload does, and fell back 3 of 3; a reloaded second tab took its sibling's pair, 5 of 5. (Back fell
back 3 of 3 too, but with Playwright's back/forward cache switched off; with it on, `main` restored
the page on Back 3 of 3 and never met the race.)

**The tab that held the pair polls** (a `sessionStorage` claim with no Web Lock). A new tab arriving
as a busy old worker was killed took the pair 13 of 16; a duplicated tab took its original's 6 of 6;
a tab back at the app after another took over waited 3 s and took that tab's pair on its reload,
5 of 5. The claim could not say *when* the holder left, nor whether anyone had taken over since, and
polling could not stop a newcomer's single try landing between two polls.

**A Web Lock held by the worker rather than the page.** It would release exactly when the files
do, which is tidier. Not built, because the successor could only ask for it once its own page and
worker had started, where the page asks from its first script — a wider window for a newcomer to
land in. Not measured either way.
