# ADR 0033 — What the camera offers is reported, and never used to decide what to ask for

**Status:** accepted

## Context

[ADR 0022](0022-a-lock-is-confirmed-before-a-burst-and-released-after-it.md) made the page read the
track's settings back, so the `locks` row says what a burst is actually holding.
[ADR 0031](0031-a-lock-is-asked-for-one-at-a-time.md) then fixed *how* the lock is asked for, after
the first device reading came back `focus · exposure refused · white balance refused`.

That row says a lock did not take. It does not say whether there was one to be had. Those are
opposite problems — a camera that advertises `manual` and then will not take it is a browser or
firmware defect to work around, and a camera whose only exposure mode is `continuous` is a burst
that will never have a fixed exposure and a `FrameQualityEngine` that must be told so. We have now
guessed twice at which of them that Pixel is, and been wrong twice: once by asking for all three
locks in one constraint set, once by asking for `manual` with no exposure time. Each guess cost a
device round trip to disprove, on hardware that is not on this desk.

`MediaStreamTrack.getCapabilities()` answers the question directly, where it is implemented. It is
optional, a track may not have it at all, and a browser that does fills the dictionary in as it
likes — measured against the browser this repo's suite pins, Chromium's fake device lists
`["manual", "continuous"]` for `exposureMode` and for `focusMode`, and has no `whiteBalanceMode`
key at all.

## Decision

**The track's mode lists are read when the camera opens, and reported.** `CameraAccess` grows
`offeredModes()`, filled from `getCapabilities()` at `open` and cleared with the track at both ends
of its life, for the same reason ADR 0031's remembered refusals are: what a camera offers is a fact
about *that* camera. The capture client renders it against a refusal, so the row reads
`focus · exposure refused (offers continuous, manual)` — the refusal, and the evidence it should be
read against.

**Not knowing is a distinct answer from knowing there is nothing.** Three things mean the browser
did not tell us — no `getCapabilities` on the track, a call that throws, a dictionary without the
key — and all three report as `null`, which renders as `not reported`. A camera that lists only
`continuous` renders as `offers continuous`. Collapsing those two into one absence is the thing
that made `refused` ambiguous in the first place, and doing it one layer further in would only move
the ambiguity.

**A camera that will not answer is still a camera.** A `getCapabilities` that throws used to escape
into `open`'s own catch and report a working camera as unavailable — the app dead for a device
whose only fault was declining a question asked for a status line. It is now caught where it is
asked.

**None of it gates what the camera is asked for.** The negotiation of ADR 0031 is untouched: every
lock the client wants is still asked for `manual`, then `single-shot`, and believed only when the
settings read back say so. Three reasons, in the order they matter:

- **Browsers under-report.** A device may take a constraint it never advertised, and a track that
  answers nothing would get *no* locks at all from a negotiation that read the list first. That is
  not hypothetical arithmetic: the iPhone holds a white balance lock today, and the record does not
  say whether that camera advertised the mode or merely accepted it — a lock that is held reads the
  same either way. Gating on the list would remove that lock in one of those two cases, and remove
  it silently. Which case it is is precisely what this change makes visible; deciding it in advance
  is the mistake being corrected.
- **The evidence is one-directional.** A list containing `manual` does not mean the lock will take:
  that is exactly the Pixel, and it is why ADR 0022's read-back exists. A list without it means no
  more than the browser's willingness to enumerate. Neither direction is worth trusting over
  asking.
- **Asking is nearly free.** ADR 0031 already bounds it: at most two round trips per lock per
  track, remembered after the first refusal.

**Nothing crosses to the worker.** `CameraCapabilities` carries the three `supports…Lock` booleans
the core plans with, and no component in the core would decide anything with a list of browser mode
strings. Adding the field would be a contract change made to carry a value straight back to the
page that produced it.

## Consequences

- **The row now says which problem it is looking at.** A refusal reads
  `exposure refused (offers continuous, manual)` where the camera contradicted itself,
  `exposure refused (offers continuous)` where there was never a lock to take, and
  `exposure refused (not reported)` where the browser would not say. With nothing held and nothing
  asked for, `none — this camera offers no manual modes` is now claimed only when all three lists
  were actually reported; a browser that answered none of them reads
  `none — this browser does not report what the camera offers`, and one that answered some says
  which is which.
- **A held lock says nothing extra.** The row is read at a glance between cells, and what a lock
  that is holding *could* have been is not a question anyone has.
- **The annotation reachable on a phone today is the self-contradiction one**, because the client
  still asks for exactly the locks `Open` reported as supported (ADR 0022), so `wanted` implies the
  list contained `manual` or `single-shot`. The other two readings are reachable through
  `describeLocks` and are covered, which is what makes the client's gate safe to revisit on its own
  merits later: the row already says the right thing on the day it is asked for a lock nothing
  advertised. Whether that gate should go is a question for a device reading, not for this change.
- **`getCapabilities` is now read for two purposes with one call**, and only one of them is
  business: `supportsExposureLock` and its siblings still come from `offersManual`, unchanged, and
  are still what the client asks with. The mode lists sit beside them and decide nothing.
- **The iPhone's row becomes readable too.** ADR 0031 recorded that camera as offering no manual
  or single-shot mode for exposure or focus at all, which was inferred from two `supports…Lock`
  booleans being false — and a browser that answers nothing produces exactly those booleans. That
  inference stands or falls on which it was, and the row now says.
- **The next screenshot from that Pixel answers a question rather than raising one.** If the lists
  hold `single-shot` and the lock still refuses, the fallback ADR 0031 added does not work there
  and the row will say so plainly.

## Rejected alternative

**Ask only for what the camera advertised.** The obvious use of a capability list, and it would
save the wasted round trips on a camera that has nothing to give. It is rejected for the reason the
decision names: the evidence is not good enough to act on in either direction, and the cost of
being wrong is asymmetric. A gate that is wrong takes a working lock away and reports neither a
lock nor a refusal — the burst simply meters through itself, which is the failure ADR 0022 exists
to make visible, restored one level up. A gate that is right saves at most two `applyConstraints`
calls per track. If a device reading later shows a camera whose list can be trusted, the gate can
be argued from that reading rather than from the shape of the API.

**Widen `CameraCapabilities` and push the lists to the core.** It would put the whole camera
picture in one place, and the core is where every other decision about the camera is made. Nothing
in the core would read it: the plan is sized from the field of view, and `SetLocks` compares a
request against booleans the page confirmed. A contract field exists to let a component decide
something, and this one would only travel out and come back.

**Summarise instead of listing — "offers no manual mode", "advertised manual".** Shorter, and it
reads better in a row that has to fit on a phone. It is a judgement made at the wrong end: the
whole reason this exists is that we twice decided in advance which distinction mattered and twice
picked wrong. Naming the modes verbatim costs a few characters and leaves the reading to the
reader — including for a mode nobody here has thought about yet.
