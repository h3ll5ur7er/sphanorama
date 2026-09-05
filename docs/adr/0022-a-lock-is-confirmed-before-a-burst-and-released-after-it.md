# ADR 0022 — A camera lock is confirmed before a burst and merely posted after it

**Status:** accepted

## Context
`ICameraAccess::SetLocks` has refused since it was written. `BrowserCameraAccess` returned
`Unsupported` for any lock, `Open` reported `supportsExposureLock = false` regardless of the
camera, and the capture client armed bursts with every lock off — because asking for one would
have meant no burst at all.

That was honest and it was expensive. A burst exists so that selection can compare several frames
of the same view and keep the sharpest. Comparing frames that differ in exposure ranks them on
brightness instead, and the cell then blends with banding nobody can trace back here.
[ADR 0019](0019-the-core-runs-in-a-worker.md) recorded the reason and left it open:

> **`SetLocks` is not one of them**, and it is worth being explicit about why, because it looks
> like one. A lock is applied with `MediaStreamTrack.applyConstraints()`, which is asynchronous
> and can reject. `ArmBurst` calls `SetLocks` and the burst's first frame arrives on the very next
> tick, so a posted lock could easily be applied *after* the frame it was supposed to govern — and
> a rejection would vanish… Either the page applies the locks and the result becomes resident
> state the core reads like any other, or the call needs an acknowledgement before arming can
> proceed.

## Decision
**The first of those, plus one asymmetry: taking a lock is a read of confirmed state, releasing
one is a posted write.**

- **The page applies and confirms.** `camera.setLocks()` asks the track for the manual modes the
  request implies, then **reads the settings back** and reports what the camera actually settled
  on. That confirmed state is pushed to the worker and becomes resident state like the camera's
  capabilities and the preview frame before it.
- **The client applies before it arms.** Arming already happens inside the capture loop
  ([ADR 0021](0021-the-pixel-path-crosses-by-transfer-and-lands-in-the-frame-store.md)), so the
  ordering has somewhere to live: apply, confirm, push, then `ArmBurst` — and the burst's first
  frame lands a tick later, against a lock that was already in force.
- **`SetLocks(true, …)` reads, and refuses what is not held.** It does not ask the page for
  anything; it compares the request against what the page confirmed, and returns
  `FailedPrecondition` naming the locks that are missing. So a client that forgot to apply them
  gets a burst that will not arm, rather than a burst that quietly compares brightness.
- **`SetLocks(false, false, false)` posts and returns `Ok`.** A release is a write, and ADR 0019's
  rule for writes applies: nothing is waiting on it, the burst that held the lock is over, and a
  lock released a few milliseconds late costs nothing. The asymmetry is the decision — the two
  directions have genuinely different failure modes and the same treatment for both would be
  wrong in one of them.

**Reading the mode back is the load-bearing part.** `applyConstraints` resolving means the browser
accepted the request, not that the mode changed; implementations differ, and on the ones where it
does not take, a caller told "applied" fires a burst believing its exposure fixed. The only thing
that makes the claim true is asking the track afterwards what it is doing.

**Capabilities are reported, not assumed.** `Open` now fills `supportsExposureLock` and
`supportsFocusLock` from `getCapabilities()` — a lock is offered when the track lists a `manual`
mode for it. The client asks for exactly what is offered and no more, so a desktop webcam with no
manual exposure gets a burst with the locks it can have and a line on screen naming the ones it
could not.

## Consequences
- **A burst on a phone is now comparable frame to frame**, which is the difference between
  `FrameQualityEngine` being able to do its job and not. It is the last thing that had to exist
  before that engine is worth writing.
- **A camera that cannot lock still captures.** Degraded and said out loud: the guidance line
  reads "capturing without exposure lock". That is better than both alternatives — refusing to
  capture, or capturing while claiming a lock that is not held.
- **What actually settled is on the status list**, as a `locks` row. The read-back was already
  being computed for the arming call and then discarded, which left the one question a burst's
  numbers raise unanswerable from a screenshot: on a Pixel, one cell's five candidates scored
  1186, 1180, 459, 458, 458 — two frames from one regime and three from another — and nothing on
  screen said whether the camera had been free to re-expose and refocus across the third of a
  second the burst spans. Held and refused are named separately: a refused lock is a camera that
  advertised a manual mode and then did not take it, which is the case this read-back exists for,
  while one never asked for is a camera that said up front it has none. A third case reads
  `unknown`: asking is itself fallible — a track pulled away mid-gesture leaves no camera to put
  the question to — and rendering that failure as three absent locks would make the row claim the
  camera has no manual modes, which is exactly the reading it exists to make trustworthy.
- **The client sequences a resource access directly**, which looks like a layer violation and is
  not: the shell's page-side adapters are the state ADR 0014's resident host is made of, and the
  client has always called `camera.open()` and `motion.start()` the same way. What it must never
  do is call an *engine* or reach past a manager for a decision, and applying a constraint is
  neither.
- **The page-to-worker protocol gains its fifth push and second callback.** ADR 0019 said that
  past a handful of messages it wants generating. This is the message that makes the case; the
  next one should come with a generator rather than by hand.
- **`BurstSpec`'s three lock flags now mean something**, and the contract's `CameraCapabilities`
  has only two of them — there is no `supportsWhiteBalanceLock`. The page-side type carries all
  three and the client uses it, so nothing crosses a contract that cannot describe it. Adding the
  third field is a contract change worth making when something in the core needs to reason about
  it, and nothing does yet.
- **Asking is its own problem, which the read-back exposed.** The first device reading came back
  `focus · exposure refused · white balance refused`, and the frames agreed: the sharpness cliff
  in a burst fell on exactly the candidates whose exposure agreement had dropped. Three things
  were wrong with how the page asked, and none of them was visible until this row named a refusal.

  **One constraint set per lock, not all three in one.** An advanced constraint set is applied
  only if the *whole* of it can be satisfied, so a mode the camera will not take silently
  discards the ones it would have. That camera had refused the exposure and been recorded as
  refusing the white balance too.

  **An exposure time offered alongside `manual`.** On Android `manual` largely means "and I will
  tell you the number"; asked for on its own it is refused. The number to offer is the one the
  camera is metering at when the cell is framed, which is the exposure the burst wants held.

  **`single-shot` as a fallback.** The weaker promise — converge once, then hold — costs nothing
  to make and is all a burst needs, so giving up after `manual` left cameras metering for no
  reason. It counts as a lock on the way back in too: what matters is that the camera has stopped
  moving, not which word it used.

  A refusal is remembered per track, because locks are applied before every burst and a sphere is
  twenty-eight of them; a camera that said no once will say no every time, and each attempt is a
  round trip between framing a cell and capturing it.

## Rejected alternative
**Lock once for the whole session.** The page could apply the locks when capture begins and hold
them until it ends, which removes the ordering problem entirely — nothing is applied mid-session,
so nothing can be applied late. It was rejected because a sphere spans the sky and the ground, and
one exposure for all of it blows out half and crushes the other. Per-burst locks are what
`BurstSpec` already describes, and they are right: fixed *within* a cell, free to meter *between*
them, with cross-cell differences left to `CompositionEngine`'s gain maps in Phase 2.

**Acknowledged arming: `ArmBurst` returns a promise the page resolves once the constraint lands.**
This is the shape ADR 0019 named as the alternative, and it would work. It was rejected because it
puts asynchrony back inside a manager call — the thing ADR 0014 exists to prevent — to buy an
ordering the client can establish for free by applying first. The manager would gain a state it
can be suspended in, and every other caller of `ArmBurst` would inherit it.
