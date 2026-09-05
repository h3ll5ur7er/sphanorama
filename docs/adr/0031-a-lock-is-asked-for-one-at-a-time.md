# ADR 0031 — A lock is asked for one at a time, with a fallback, and a refusal is remembered

## Context

ADR 0022 established that a burst applies its locks before arming and confirms them by reading the
track's settings back, because `applyConstraints` resolving says the browser accepted a request and
not that the mode changed. That read-back did its job on the first device that disagreed with us:
a Pixel reported `focus · exposure refused · white balance refused`.

The frames agreed. One cell's five candidates scored 1174, 1238, then 1.375, 1.186, 1.183 — and
the cliff fell on exactly the frames whose exposure agreement had dropped, 1.00, 1.00, 0.67, 0.78,
0.84, climbing back as the metering settled. Not the scene and not the selection policy: a camera
re-metering through a burst that believed its exposure was fixed.

So the read-back was right and the *asking* was wrong. ADR 0022 said nothing about how to ask —
there was no reason to think it was interesting — and three things about it were.

## Decision

**One advanced constraint set per lock, never one set for all three.** An advanced set is applied
only if the whole of it can be satisfied. Asking for exposure, white balance and focus together
means one mode the camera will not take discards the two it would have — which is what that Pixel
did. It had refused the exposure alone, and the row reported a real refusal and a false one in the
same breath.

**`manual` goes with the exposure time the camera is already metering at.** On Android `manual`
largely means "and I will tell you the number"; asked for on its own it is refused. The number to
send is the one in the track's current settings, which is the exposure the cell was framed at and
the one the burst wants held.

**`single-shot` is the fallback, and counts as a lock.** It is the weaker promise — converge once,
then hold — and it is the whole of what a burst needs. Giving up after `manual` left cameras
metering for no reason. It is read back as a lock for the same reason it is asked for: what matters
is that the camera has stopped moving, not which word it used. `CameraCapabilities` widens to
match, so a camera offering only `single-shot` is no longer reported as having no lock at all.

**A refusal is remembered for the life of the track.** Locks are applied before every burst and a
sphere is twenty-eight of them; a camera that says no once will say no every time, and each attempt
is a round trip sitting between framing a cell and capturing it. The memory is cleared at both ends
of a track's life — `open` as well as `close` — because what a camera will not do is a fact about
that camera, and carrying it across is how the next one silently loses a lock it would have given.

## Consequences

- **The `locks` row means what it says.** Before this, a refusal in one mode could be reported
  against modes the camera had never been asked about — so the row that exists to make a burst's
  numbers attributable was itself unreliable in exactly the case it was built for.
- **Up to two round trips per lock on a camera that refuses**, once, and then none: the memory is
  what keeps a refusal from costing a delay before every burst for the rest of the sphere.
- **The read-back is now load-bearing twice.** It confirms the lock, and it is the loop's own exit
  condition — the next mode is only tried if the settings still say the camera is adapting.
- **Nothing in the core changes.** `SetLocks` still receives what settled, `BurstSpec` still
  carries what was asked, and a camera that can lock nothing still captures. This is entirely a
  question of how the browser adapter phrases a request.
- **An iPhone reads `white balance` and nothing else**, which is a different shape of the same
  problem: that camera offers no manual or single-shot mode for exposure or focus at all. There is
  nothing to negotiate there, and the row says so honestly.

## Rejected alternative

**One ordered list of constraint sets, letting `advanced` pick.** The spec's `advanced` is a list
precisely so a caller can offer alternatives, so `[{manual, exposureTime}, {manual}, {single-shot}]`
looks like exactly the right shape. It is not: the UA applies *every* set it can satisfy, in order,
rather than stopping at the first — so a camera that can do both `manual` and `single-shot` would
end on `single-shot`, the weakest of the three, chosen by list position rather than preference.
Separate calls with a read-back between them say what is meant.

**Ask for `single-shot` first, since it is the one more cameras take.** It would usually cost one
fewer round trip. It also gives up a fixed exposure on every camera that would have granted one, to
save a few milliseconds before a burst — which is the wrong trade for the thing the burst exists to
measure.
