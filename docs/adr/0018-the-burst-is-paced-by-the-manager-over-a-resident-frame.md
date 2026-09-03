# ADR 0018 — A burst is paced by the manager over a resident preview frame

**Status:** accepted

## Context
ADR 0014 made every resource-access port synchronous over a host that keeps its state resident,
and left exactly one thing open: *"a burst takes time and cannot be made resident in advance, so
`ICameraAccess::CaptureBurst` will need either Asyncify on that call path or a redesign in which
the client drives burst timing. That is a decision to take with measurements."* Since then
`BrowserCameraAccess::CaptureBurst` has refused with `Unsupported`, and the cells stay empty.

Those are the measurements. Emscripten 6.0.9, single-threaded release build, headless Chromium.

**Size**, gzipped, which is what the budget counts:

| | baseline | `-sASYNCIFY` | delta |
| --- | --- | --- | --- |
| `sphanorama-core.wasm` | 29,414 B | 32,621 B | +3,207 (+10.9%) |
| `sphanorama-core.js` | 4,626 B | 6,145 B | +1,519 (+32.8%) |

**Latency**, median of nine interleaved rounds of 200,000 calls, both modules resident in one page
and measured alternately — measured in separate runs the machine drifted by 60% and the comparison
was worthless:

| | baseline | `-sASYNCIFY` | delta |
| --- | --- | --- | --- |
| trivial wasm export | 14.5 ns | 29.5 ns | +15.0 ns |
| a real facade round trip | 138.0 ns | 168.0 ns | +30.0 ns |
| an `EM_JS` call into the page | 26.0 ns | 42.0 ns | +16.0 ns |
| one suspend and resume | — | 1,085 ns | (floor: 155 ns for a bare `await`) |

**Both costs are affordable and neither decides this.** The wasm budget has 8.36 MB of headroom;
`OnMotion` at 60 Hz would pay an extra 1.8 µs per second; a suspend is a microsecond against a
burst measured in hundreds of milliseconds. ADR 0014 guessed binary size would be the blocker. It
is not.

What decides it is behaviour, and it took three experiments to see:

1. A live C++ local carried across a suspend comes back intact. Asyncify restores the frame.
2. A **non**-suspending facade call re-entering while a suspend is pending is safe: it returned a
   correct encoded `Result`, and the suspended call still resumed with the right value.
3. **Two overlapping suspends hang the main thread and then crash the renderer.** Not an
   exception, not a status code — the tab dies and every later call reports `Target crashed`.

And the instrumentation cannot be contained to the one call that needs it. `-sASYNCIFY_ADVISE`
reports every manager method as able to change the state — `ProjectManager::List`,
`CaptureSessionManager::OnMotion`, all of them — because `sph_facade_call` dispatches through
`dynCall_*` and the conservative scan cannot see through an indirect call. The generated facade
that ADR 0009 and ADR 0012 gave us is precisely what makes Asyncify all-or-nothing here.
`ASYNCIFY_ONLY` would narrow it by hand, and a wrong list there is memory corruption rather than
a failed build.

So Asyncify would make the entire manager surface unwindable to buy one call, and impose a
one-suspend-at-a-time rule whose failure mode is a dead tab.

## Decision
The burst is **paced by the manager across the ticks the client already makes**, over a preview
frame the page keeps resident. This is ADR 0014's pattern applied to pixels rather than an
exception to it.

- `ICameraAccess::CaptureBurst` is removed. `PeekPreviewFrame()` — already in the contract, already
  documented as "latest preview frame, borrowed, valid until the next call" — becomes the only
  route to a frame.
- `ICaptureSessionManager::CaptureCell` stops returning a finished burst and instead **arms** one.
  The armed burst is session state, held by the manager, which is where session state belongs.
- While a burst is armed, each tick takes one frame, scores it, and appends a candidate.
  `CaptureGuidance.action` reports `Firing` until the burst is full and then `CellDone` — two enum
  values that already exist and are unreachable today.
- `BurstSpec` is unchanged. The manager still decides how many frames, at what interval, and which
  locks; the client decides nothing and never touches a frame.

## Consequences
- No component learns that a burst takes time. Pacing across ticks is sequencing, and sequencing
  is what a manager is for — the alternative was making every manager a state machine over
  asynchrony, which is the outcome ADR 0014 exists to prevent.
- The client's job does not grow, but it does gain an obligation. It already calls `onMotion` on
  the animation frame — except that it skips the call when no sample arrived, because an empty
  batch cannot move the pose. A burst advances on that tick and on nothing else, so while one is
  firing the skip is no longer an optimisation: it stalls the burst, holding the exposure lock,
  on exactly the devices where samples are intermittent or absent. The pump therefore ticks while
  the last guidance said `Firing`. **The hole that leaves is the first tick after arming**, which
  no guidance has reported yet — whatever calls `ArmBurst` has to make sure a tick follows it.
  That is a sharp edge, and the way to blunt it is to route arming through the same loop rather
  than trusting each caller; nothing arms a burst from the client yet, so it is recorded here
  rather than solved on speculation.
- **`BurstSpec::intervalMs` becomes a floor, not a cadence.** Frames are taken at most one per
  tick, so at 60 Hz nothing below ~16.7 ms can be honoured. The default burst — five frames at
  80 ms — is ~400 ms and sits comfortably above that, but a spec asking for 5 ms between frames
  will silently get 16.7. Reporting that honestly is better than a call that blocks and lies.
- **The camera's own rate is a second floor, and it is the one that bites.** `PeekPreviewFrame`
  borrows the *latest* frame, so ticking faster than the camera produces frames does not capture
  faster — it captures the same frame twice. On a 30 fps camera in a 60 Hz loop, a burst asking
  for 0 ms would fill with duplicates and selection would rank one exposure against copies of
  itself, which is worse than a slow burst because it looks like a fast one. So the manager keeps
  `CameraCapabilities::maxBurstFps` from `Open` and takes the larger of the two periods.
  `maxBurstFps` is 0 when the platform will not say, and then the tick rate is the only floor
  there is.
- **The exposure lock now spans ticks.** `SetLocks` is applied when the burst is armed and must be
  released when it completes *or is abandoned* — a session that ends mid-burst, a retake that
  re-arms, a cell that fails scoring, and every early return from a tick, since the burst is
  advanced at the end of one and a pose or planner failure returns before it. A burst that leaves
  the camera locked is a viewfinder the user cannot fix by pointing somewhere else, and the
  release is fallible, so its failure is reported rather than discarded: the cell is captured all
  the same, and the caller is told the camera is still locked instead of finding out later. On the
  paths where the release coincides with another failure the two are combined rather than one of
  them winning — the cause keeps the status code, and the unlock's detail rides along, because
  a caller told only "the pose engine failed" has no reason to think the exposure is still pinned.
  `End` is the exception, and deliberately: it closes the camera immediately afterwards, and a
  close that succeeded took the locks with it, so a failed unlock there is moot rather than
  hidden — it is reported only if the close also failed.
- **A burst's frames are not in the cell until the whole burst ranks.** They are held apart and
  appended on commit, which is a semantic worth stating: `Candidates` and `Coverage` do not see a
  burst in flight. `Evaluate` counts a cell satisfied as soon as one candidate exists for it, so a
  cell that filled in public would report itself complete on the first frame of a burst that could
  still roll back — and a sphere could read as finished while holding a cell nothing ever ranked.
  It also makes the rollback exact: `OfferFrame` appends to the same cell, and an index into it
  was never an ownership boundary.
- `PeekPreviewFrame` moves onto the hot path, so an allocation per tick is the pattern
  `IFrameStoreAccess` has to be designed around rather than a burst-sized batch.
- `CaptureCell`'s signature changes, so the TypeScript mirror, both codec halves and the facade
  dispatch regenerate. Nothing changes shape at the boundary.
- No Asyncify means no JSPI question either: the single-threaded build stays as it is, and iOS —
  which has neither — is not a special case.

## Rejected alternative
**Asyncify on the burst path.** It was the obvious answer and the numbers say it is cheap, which
is why the interesting part is that cost was never the problem. It was rejected because our own
generated facade makes the instrumentation all-or-nothing, and because the rule it imposes — one
suspend in flight — is enforced by hanging the main thread and crashing the renderer rather than
by returning a status. A guard in the facade could hold that rule, but it would be a guard whose
bug is a dead tab, protecting an ability we do not otherwise want.

Two smaller ones. **Routing the burst through `OfferFrame`** was the first shape considered and is
wrong: that call exists for externally sourced frames — file import, replayed datasets, a manual
shutter — and using it for the burst would put pixel ownership and `FrameRef` handling in the
client, which is what `IFrameStoreAccess` exists to prevent. **Asynchronous contracts** were
already rejected in ADR 0014, for the reason that has not changed: they solve the mechanical
problem by spreading the volatility the architecture exists to contain.
