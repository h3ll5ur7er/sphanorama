# 0045 — A capability that changes is re-asked, not remembered

## Context

`CameraCapabilities` is answered once, by `ICameraAccess::Open`. `CaptureSessionManager` copies what
it needs into `max_burst_fps_` at `Begin` and at `Resume`, and nothing invalidates either copy for
the life of the session.

That was harmless while the field was dead. It stopped being harmless the moment PR #49 wired
`maxBurstFps` through to the browser for the first time, and a reviewer pointed out that the app
changes the very thing it had just started trusting:

- `maxBurstFps` floors a burst's interval and its settle (ADR 0018, ADR 0032). `PeekPreviewFrame`
  borrows the *latest* preview frame, so a burst asking for frames faster than the camera makes
  them fills with duplicates of one exposure, and selection ranks a frame against copies of itself.
- `SetLocks` drives `applyConstraints({ advanced: [{ exposureMode: 'manual', … }] })` (ADR 0022).
  A camera whose exposure has just been pinned long is exactly the one that drops from 30 fps to
  15 — that *is* what an exposure lock in dim light does.

So the floor goes stale in precisely the direction that reintroduces the defect it exists to
prevent, and only while a burst is running, which is the only time it matters.

The question this raises is bigger than one field. `maxWidth` and `maxHeight` have the same shape
the day a track renegotiates resolution, and `supportsExposureLock` the day a platform reports a
mode it later withdraws. Answering it for `maxBurstFps` alone would leave the next field to be
found by the next reviewer.

## Decision

**A capability is re-asked when it is about to be used, not remembered from when the device was
opened. `ICameraAccess` grows a read-only `Capabilities()`, and `CaptureSessionManager::ArmBurst`
calls it before it computes a burst's timing.**

**And the port it asks has to have something new to say, which is the half the first version of
this ADR got wrong.** A resource-access port is resident (ADR 0014): `capture-host` runs inside the
worker and the `MediaStream` lives in the page, so the host cannot read a track — it answers from
what the page last pushed into it. A pull therefore reaches the page's *cache*, not the device, and
a pull-only design was never available to us. Three reviewers found this independently within
minutes of each other, and one put it exactly: the ADR rejected a push because it "makes correctness
depend on a client noticing", and as first built the pull depended on precisely that.

So the pull and the push are complementary rather than alternatives, and each owns a different
question. **The pull decides *when* a value is read** — at the moment it is consumed, which is what
keeps the rule general across capabilities. **The push is how a resident cache stays true** — the
page re-reports the camera after it changes it, which today means after `setLocks` settles, since
that is the call that reconfigures the track. Neither is sufficient: without the push the pull reads
a stale cache, and without the pull a fresh cache is read at the wrong moment.

Three things follow.

**`Capabilities()` is a read, not a second `Open`.** It reports what the device is doing now and
has no side effects: it does not acquire, does not prompt, and does not change what the preview is
delivering. A port that has a camera and has not opened it returns
`FailedPrecondition`; a port with no camera at all returns `CameraUnavailable`, which is what
`NullCameraAccess` does for everything.

**`ArmBurst` is the place, because arming is the moment the numbers are consumed.** The burst's
interval and settle are computed there and nowhere else, and it is the one call in the sequence
that happens *after* the locks have been applied — which is the event most likely to have changed
the answer. Re-asking on every `OnMotion` would put a port call on the tick loop for a value that
changes at most once per burst; re-asking at `Begin` only is what we have.

**A refusal to answer is not a refusal to arm.** If `Capabilities()` fails, the burst goes ahead on
what the session already had. The alternative — declining to capture because a status line's number
could not be refreshed — trades a real capture for an accurate figure, which is backwards.

The capabilities the manager last read are exposed on the facade, as
`ICaptureSessionManager::CameraInUse()`.

## Alternatives considered

**`SetLocks` reports revised capabilities.** The invalidator is already in hand: `setLocks` in the
shell adapter re-reads `track.getSettings()` three lines after applying the constraints, on the same
object that carries `frameRate`. Rejected because it puts the answer in the wrong *contract*.
`ICameraAccess::SetLocks` answers "which locks are held", and widening its return value to "and here
is everything else about the camera" would make a lock write the only channel capability news can
travel down — a session that never locks anything could never learn, and no other cause of change
would have a way in.

A reviewer read this alongside the shell and asked the obvious question: `main.ts` pushes the
capability set immediately after the lock write settles, so is the lock write not the channel after
all? It is the *occasion*, and that is the distinction this rejection turns on. The page pushes
because it has just changed the camera, and it may push whenever else it learns something —
`camera.capabilities()` is a read of the track, callable from a `configurationchange` listener or a
thermal event with nothing else moved. Had the answer ridden home on `SetLocks`'s return value there
would be no such second door: a fact would be reachable only through the call it was bolted to. The
occasion is one; the channel is general.

**A push from the page, instead of a pull.** ~~Rejected because it makes correctness depend on a
client noticing.~~ **This rejection was wrong and is withdrawn.** The objection stands as an
objection — a host that forgets to re-report is a silent failure — but it is not a reason to choose
the pull *instead*, because the pull cannot reach past the worker into a `MediaStream` either. Both
are needed, and the decision above says so. What the objection does buy is where the test goes: the
push is the fallible half, so it is the half a browser test has to pin.

**Nothing — keep the snapshot and document it.** Rejected because the documented behaviour would be
"the burst floor is right unless you locked the exposure", and the whole point of the floor is the
burst that runs after the locks are applied.

## Consequences

**A contract grows a method, and every implementation owes an answer** — the *same* answer for the
same state, which is what a contract is. `camera_access_contract_test.cpp` states it, which is where
the first version of this decision had nothing at all and three implementations gave three answers.

"States" rather than "holds them to it", and the difference is worth being exact about, because a
reviewer found the stronger claim and it is not true: the typed suite has one entry, and it is
`FakeCameraAccess`. `NullCameraAccess` cannot join it — refusing every call is its whole job — and
`BrowserCameraAccess` cannot either, since its body is `EM_JS` and it runs under wasm and nowhere
else. So the suite says what the rule is and holds one implementation to it. The browser one is held
by the browser suite instead (see the next consequence), and the null one by its own test in the
same file.

Two refusals, and they are different facts rather than two spellings of one: a port that has a
camera and has not opened it answers `FailedPrecondition`, a call out of order and fixable by
opening; a port with no camera at all answers `CameraUnavailable`. `NullCameraAccess` is the
second, and an earlier draft of this ADR asserted it was the first — wrongly, and about the port
whose whole job is to refuse. `FakeCameraAccess` answers from what a
test set, and `BrowserCameraAccess` reads the metrics it already reads at `Open`, through one shared
helper so the two calls cannot compute different values.

**The camera seam becomes testable, which it was not.** `host_camera_metric` and the page agree by
an integer index and a property name, and nothing checked either: `maxBurstFps` was in the C++
struct for the life of the field, had no `case` in the switch, and no suite could tell. With
`CameraInUse()` on the facade a browser test opens a real camera and reads every capability back
through the boundary, so a missing case or a renamed property fails a test instead of reading as
zero.

Seven of the eight cases, to be exact, and the eighth is a hole in the runner rather than in the
assertion: the test compares what the core reports against what the page reads off the same track,
and Chromium's fake camera has no torch — so `supportsTorch` is `false` whether the metric is read
or not, and equality cannot separate them. Measured by renumbering each case in turn. A second
browser test covers what identity cannot: it pins an exposure, which drops the fake camera from 30
fps to 15, and asserts the core paces the burst by 15 — which fails if the page stops pushing, if
`case 8` moves, or if `ArmBurst` stops re-asking.

That is the arrangement the motion port next door already has — a named constant, the field order
written out on both sides, "pinned together by a test on each side" — and the camera port did not.

**A stale figure is still possible, and is now bounded.** Between two arms the manager's copy can be
wrong, and the status row the page draws from it can be wrong with it. What cannot be wrong is the
number a burst is actually paced by, which is the one that costs frames.

**One more port call per burst.** Synchronous, resident, and once per arm rather than per tick —
against `applyConstraints` in the same call, which is measured in hundreds of milliseconds, it does
not register.

**The refresh takes the whole struct, and `CameraInUse()` reports the camera rather than the plan.**
One rule beats a list of fields somebody has to keep current. It does mean `CameraInUse().maxWidth`
answers "what the camera says now" and not "what the plan was sized from" — those are two facts and
the second lives in `CaptureSessionManager::lens_`, the manager's own `Intrinsics`, written at
`Begin` and restored at `Resume` and deliberately never refreshed. (Not "the plan's own
`Intrinsics`", which an earlier draft said and a reviewer checked: `CapturePlan` is `{nodes, spec}`
and carries no intrinsics at all. The header already had it right.) It is never refreshed because a
camera that changes resolution mid-session has invalidated the plan, and silently adopting the new
numbers would hide that rather than handle it. Handling it is a change of its own.

**A rate of zero never overwrites a rate we had.** Zero means "the platform will not say", so a
refresh that answers `Ok` with zero is telling us the same thing a refusal does, and the two are
now treated alike: the session keeps the floor it was given. They were not, at first — a refusal
kept the old rate and an `Ok(0)` discarded it, which is two spellings of one fact with opposite
outcomes, and the browser port is the most likely producer of the second.

**The push needs a guard the pull never did, and it is on the page.** A pushed fact can arrive late.
An arm parks on `applyConstraints` for as long as that takes, and an `End()` inside that window
closes the camera underneath it — so the push lands after the host has cleared its copy and hands
the core a camera back: `cameraOpen()` reads true again, and the next `Begin` succeeds where it owed
`CameraUnavailable`, planning a whole tessellation against a struct read off a dead track (measured:
32 cells, `maxWidth 0`). The page refuses to push when it is no longer holding the camera, asked of
the tracks rather than of a flag, because `track.stop()` fires no `ended` event and the flag that
records a camera *taken* away is deliberately silent about one the core closed. Two lines of it are
the adapter's: `camera.capabilities()` answers zeros for an ended track rather than the mixture a
browser gives — geometry dropped from `getSettings()`, every mode still listed by
`getCapabilities()` — because half an answer is worse than none when none is a state the core has a
word for.

**This does not settle every capability.** The rule is settled — a capability is re-asked where it
is consumed, and the port it asks is kept true by the client that changes it — so the next field to
move has an answer rather than a discovery.
