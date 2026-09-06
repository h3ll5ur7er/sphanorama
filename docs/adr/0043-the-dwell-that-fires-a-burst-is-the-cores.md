# ADR 0043 — The dwell that fires a burst is the core's, and the page only obeys it

> **Completed by [ADR 0044](0044-a-capture-needs-a-motion-sensor.md).** The dwell is unchanged.
> Two of the consequences recorded below are not: `canCapture` and the `#capture` button were kept
> here for the one device that could never reach `Fire`, and that device is now refused outright,
> so both are deleted. The dwell is the only way a burst starts.

## Context

ADR/PR #44 settled what the capture button promises by deleting it: a burst fires by itself once
the camera has been held on a cell for about two seconds, shown on the ring already drawn round the
reticle. It left one thing open on purpose — where the dwell is counted.

Two places can count it.

**The page.** The render loop already has `performance.now()` and already knows the guidance it was
last handed. Stamping the first tick that says `HoldStill` and arming when the stamp is two seconds
old is perhaps fifteen lines, with no contract change and no core state.

**`CaptureSessionManager`.** It has `IClock`, it produces the guidance, it holds the target cell,
and it is where every other capture-sequencing decision already lives — including the refusal that
this trigger would otherwise walk straight into.

## Decision

**The dwell is counted in `CaptureSessionManager`, reported on `CaptureGuidance`, and acted on by
the client.** The core decides *whether* to fire; the page still makes the call that arms, because
a burst is paced by the client's ticks over the resident preview frame and nothing else can arm one
(ADR 0018).

Concretely:

- `CaptureGuidance` gains `double heldFraction` — 0 while seeking, climbing to 1 across the dwell.
  Appended, because field order is wire order.
- `GuidanceAction` gains `Fire`, appended: the tick on which the dwell completed. The page calls
  `armBurst` on it exactly as it called it on a press, and every refusal path it already has stays
  where it is.
- The ring's fill is `heldFraction`. `OverlayInput.holding` and `RingMark.fill` are already wired
  and already say in a comment that nothing drives a middle value yet; this is what drives it.

## Why not the page

Three reasons, and the third is the one that decides it.

**It is a policy, and policies are the manager's.** "When does a burst fire?" is the same kind of
question as "which cell is the target?" and "is this cell close enough to arm?", both of which are
already answered in the core. A client that answers this one has taken a decision out of the layer
that owns it, and the second client — a replay harness, a desktop build — would have to answer it
again and could answer it differently.

**The reset condition is easy to get wrong in the shell and free in the core.** The dwell must
restart when the *cell* changes even though the action does not: a slow pan along a row of cells
holds `HoldStill` continuously while the target moves under it, and a shell counting the action
alone accumulates two seconds across three cells and fires into whichever one it lands on. That is
the shape of the very bug ADR 0041 exists to stop. The core is already comparing the target cell
tick over tick.

**A stalled pose is invisible from the page and obvious from the core.** `performance.now()` keeps
moving when the sensor stops delivering. Guidance the page is holding still says `HoldStill` about a
cell the phone may have left, and a shell dwell would mature on it and fire. The core has the
sample timestamps and `IClock` side by side, so it can decline to accumulate over an interval no
pose arrived in — which is the same distinction `PoseState.observed` was split out to make on
PR #46. The page cannot make it at all.

## Consequences

- **A contract change, and therefore a wire change.** Both additions are appends: `heldFraction` at
  the end of `CaptureGuidance`, `Fire` at the end of `GuidanceAction`, which crosses as an index.
  `tools/contract_gen.py --check` and the codec generator carry the rest.
- **The trigger and the progress ring cannot disagree**, because they are the same number arriving
  in the same message. A shell dwell would have been a second copy of a fact the core also holds,
  which is the drift the review lens is named after.
- **`#capture` goes.** The button, its handler, its disabled-state management, and the tests that
  press it. `canCapture` does not go: it becomes the answer to "may this tick arm?", which is what
  the manager asks itself.
- **The dwell length is a constant in the core, not a config key yet.** Same standing as
  `kUnusableRateRadPerSec` — a starting point, tuned once there are real captures.
- **The button survives on exactly one device, and it is the device that has nothing else.** On a
  phone with no motion sensor `aimKnown` is false, guidance never says `HoldStill` (ADR 0042), and
  so it can never say `Fire`: there is no aim to hold and no stability to measure — `Stability`
  refuses a batch with no samples rather than answering "still". A dwell on wall-clock alone would
  fire whether or not the person was ready, which is worse than the button it replaced. So `#capture`
  is shown when and only when `aimKnown` is false, and hidden the moment an aim exists. This is a
  consequence of #44's decision rather than a softening of it: the button is gone from the capture
  flow the decision was about, and what is left is the fallback for a device that has no flow.
  It also moves out of `#panel-details`, which is the other half of what was wrong with it.

## Rejected alternative

**The page counts and the core reports nothing.** Cheapest by a wide margin and no contract change.
Rejected on the stalled-pose case above: a dwell that matures on a pose that stopped arriving fires
into a cell the phone may no longer be pointing at, and the page has no way to tell. The other two
reasons alone might not have outweighed the cost; that one does.

**The core arms the burst itself when the dwell completes.** It removes the round trip and the
client's chance to disagree. Rejected because a burst is paced by the client's ticks over a preview
frame the page keeps resident (ADR 0018): the manager cannot get a frame without being called, so
"the core arms it" would mean the core arming on the same tick the client is already driving —
which is what `Fire` is, with the client's refusal paths left intact instead of buried.
