# ADR 0032 — A burst waits for the camera before its first frame

## Context

`ArmBurst` applies the locks and then deliberately backdated the pacing clock by one interval, so
that the first frame was taken on the very next tick of the capture loop — about 16 ms later. The
comment said why: *the burst starts when it is armed*. Every frame after it got the spec's full
`intervalMs`; the first got nothing.

A Pixel that holds a focus lock says what that costs. Five frames of one burst, in capture order,
scored (sharpness / exposure agreement):

| order | candidate | sharpness | exposure |
| ----- | --------- | --------- | -------- |
| 1 | #41 | **5.924** | 1.00 |
| 2 | #42 | 1145.113 | 0.95 |
| 3 | #43 | 719.873 | 0.97 |
| 4 | #44 | 582.767 | 0.98 |
| 5 | #45 | 586.374 | 0.99 |

The first frame is roughly 100× less sharp than any of its siblings, which are within about 2× of
each other. An iPhone on the same build — which takes a white balance lock and no focus lock at all
(ADR 0031) — shows nothing of the kind: its first frame, #31 at 758, sits with the rest.

The lock is what separates the two devices, and the mechanism follows from ADR 0018 and ADR 0021.
`applyConstraints` with a focus mode makes a camera re-converge to honour it, and
`PeekPreviewFrame` borrows the *latest* preview frame the page has drawn. Sixteen milliseconds
after arming, the latest frame is one taken mid-refocus — or, on a slow enough page, one the camera
produced before the constraints landed at all. So the device that succeeds at locking is the device
that throws a fifth of every burst away, and the failure is invisible: the frame is a real
candidate with a real score, and ranking simply never picks it.

Nobody has measured how long a phone camera actually takes to converge after a lock.

## Decision

**`BurstSpec` gains `settleMs`, and the first frame of a burst is not taken until that long after
arming.** The clock is no longer backdated: `ArmBurst` sets the next frame due one settle ahead,
and each frame taken sets it one interval ahead — one deadline, two ways of moving it.

It is a field rather than a reuse of `intervalMs` because the two are different physical
quantities. The interval is how far apart two frames have to be to be worth comparing — a property
of hand shake and of the camera's frame rate. The settle is how long *this* camera takes to
converge on a lock, which is a property of its autofocus. They happen to be the same order of
magnitude today; a caller that knows its device should be able to say so when they are not, and a
future measurement per device class has somewhere to land.

**The default is 150 ms, and it is a guess.** What is known is one device in one scene: a frame
16 ms after arming was unusable and a frame 96 ms after arming was not. 150 ms is that datapoint
with margin, and it is worth saying plainly that nothing else supports it. Measuring convergence
per device class is the work that would replace the number, and until that exists the field's
comment says so too.

**The camera's own frame period floors the settle**, exactly as it floors the interval, for a
related but distinct reason. `PeekPreviewFrame` borrows the latest preview frame, so within one
frame period of arming the latest frame is necessarily one the camera produced *before* the locks
landed — the cheapest form of the failure this ADR exists to fix. A caller asking for `settleMs =
0` on a camera that reports 30 fps therefore still waits 33 ms; a camera that will not report a
rate leaves the tick rate as the only floor, which is what `maxBurstFps = 0` already means for the
interval.

**A negative settle is refused with `InvalidArgument`**, for the same reason a negative interval
is: the arithmetic would make the first frame overdue the moment the burst was armed, so instead of
failing it would silently reinstate the behaviour this ADR removes.

## Consequences

- **A burst is one settle longer.** With the defaults that is 150 ms on top of 320 ms of frames, so
  a cell takes about half a second instead of a third of one, and a 28-cell sphere gains about four
  seconds of hold-still spread over the whole capture. Against that, the burst that used to hand
  selection four usable frames out of five now hands it five.
- **The locks are held over the settle**, not applied after it. That is the point: the camera has
  to be converging on the locks the burst will use, not on the ones the viewfinder had.
- **The client names the number.** The wire carries every field of a `BurstSpec`, so the page has
  to state `settleMs` rather than inherit the C++ default. It states 150 and says why.
- **It amends ADR 0018, which recorded that `BurstSpec` was unchanged** when the burst became
  paced by the manager. It is changed now, in the direction that ADR set up: the spec is where a
  burst's timings are stated, and the settle is one of them.
- **A device measurement now has a home.** "How long does this camera take to settle?" was
  previously not a question the contract could express; the answer, when someone takes it, is a
  field to set rather than a constant to argue about.
- **The guess can be wrong in both directions**, and only one is visible. Too long merely costs
  time. Too short leaves a soft first frame that still scores, still ranks, and still looks like a
  candidate — which is exactly how this went unnoticed until a device printed its numbers.

## Rejected alternative

**Reuse `BurstIntervalNs()` as the settle and simply stop backdating.** It is the smaller change —
no contract, no regeneration, no new default — and it would have fixed the Pixel, since one 80 ms
interval is most of the 96 ms that was already sharp. It conflates two quantities that only look
alike: a caller that wanted its frames 40 ms apart would get 40 ms of convergence time it never
asked about, and one that lengthened the interval to fight hand shake would silently lengthen the
settle too. The number this ADR is least sure of would then be unnameable, and the follow-up
measurement would have nowhere to go.

**Drop the first frame after the fact instead of waiting for it.** Take the burst as before and
discard candidate one, or let `Rank` learn to distrust it. This is cheaper in wall-clock time and it
is wrong twice: it throws away a frame that is perfectly good on the devices that take no focus
lock, and it puts a fact about the camera's convergence inside a selection policy whose job is to
compare frames rather than to know how they were taken (V6). A frame nobody should have captured is
not a ranking problem.
