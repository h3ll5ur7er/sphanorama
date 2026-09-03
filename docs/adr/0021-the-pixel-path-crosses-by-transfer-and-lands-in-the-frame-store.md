# ADR 0021 — Pixels cross by transfer, and the camera port is what puts them in the store

**Status:** accepted

## Context
[ADR 0018](0018-the-burst-is-paced-by-the-manager-over-a-resident-frame.md) made
`ICameraAccess::PeekPreviewFrame` the whole pixel path, and [ADR 0019](0019-the-core-runs-in-a-worker.md)
moved the core into a worker and said in one line what would fill the gap: *"the page grabs the
latest frame and transfers the buffer; the worker holds it and `PeekPreviewFrame` reads it."*

That line was written in the present tense and describing nothing. `BrowserCameraAccess::PeekPreviewFrame`
refused with `Unsupported`, the page-to-worker protocol had no message for a frame, and a burst
armed in the browser abandoned on its first tick. It stayed invisible because nothing armed a
burst from the client either — two unfinished halves lining up so that neither showed. Review
caught the claim twice, in five files.

This builds it, and in doing so runs into a layer rule and an ownership ambiguity, both of which
were latent the whole time.

## Decision

### The path
The page draws the `<video>` into a canvas, reads the pixels back as RGBA8, and **transfers** the
buffer to the worker, which holds the latest one. `PeekPreviewFrame` allocates a frame in
`IFrameStoreAccess`, pins it, copies the resident bytes in, and returns the handle.

`MediaStreamTrackProcessor` would hand the worker the track directly and skip the page entirely.
Safari does not have it, and 04 §4.1 already records that topology as the target rather than the
way in; drawing works everywhere we ship.

**The frame is capped at 1280 on its long edge.** Not a quality knob — a memory budget. RGBA is
four bytes a pixel and nothing in this path compresses anything, so a phone's 4000×3000 preview is
48 MB a frame and the default five-frame burst would be 240 MB against a browser heap ceiling of
128. At 1280 that burst is 22 MB. It costs real resolution and that is a debt: the frame that gets
*stitched* should be the full-resolution one, and paying it properly means encoding to JPEG before
the frame crosses — `PixelFormat::EncodedJpeg` is in the contract for exactly this.

**The page grabs only while a burst can use one**, because a draw plus a readback of megabytes per
animation frame for a session that is not capturing is heat and battery for nothing. That makes
the ordering a requirement rather than a preference: the frame has to be resident *before* the
tick that consumes it, since the core reads it synchronously and has nothing to wait with.

### Arming moves into the loop
ADR 0018 left this open — *"whatever calls `ArmBurst` has to make sure a tick follows it… the way
to blunt it is to route arming through the same loop rather than trusting each caller; nothing
arms a burst from the client yet, so it is recorded here rather than solved on speculation."*
There is a caller now, and it is inside the pump. The capture button and the end-to-end suite both
reach it through the same hook, so neither can make the mistake the ADR warned about.

### A peeked frame is owned, not borrowed
The contract said "the latest preview frame, borrowed, valid until the next call". That was never
what the manager did: `AdvanceBurst` keeps the `FrameRef` as a candidate for the length of a burst
and calls `Forget` on it when the burst rolls back. A port that reclaimed the frame on the next
peek would have the manager scoring bytes that had been overwritten, then forgetting a frame
twice.

So the contract now says what was already true: **each call allocates and hands over ownership**.
The contract suite's `RepeatedPeeksReturnDistinctFrames` was already asserting exactly this, which
is how the ambiguity survived unnoticed — the tests were right and the prose was not.

### The frame store is a bar, not a peer
It follows that an implementation of `ICameraAccess` reaches `IFrameStoreAccess`: a `FrameRef` is
a handle into one and there is nowhere else for one to come from. That is a resource access
depending on a resource access, which the layer rules forbid.

**The rule gains one narrow exception, allowing any resource access to depend on
`frame_store_access`** — enforced in `tools/layer_check.py`, with its own tests.

The argument is the one already accepted one layer up. Engines may touch `IComputeDeviceAccess`
and `IFrameStoreAccess` because compute placement and pixel residency are properties of the
device, not of the algorithm (docs/03 §3.3 rule 5). The same holds here: where pixel bytes live is
V11's business universally, and a port that produces pixels has to put them somewhere. This is not
a browser workaround — `FakeCameraAccess` has held a frame store since the day the contract was
written, and the native suite is what would break first if the rule were taken literally.

## Consequences
- **The pixel path is real, and an end-to-end test says so.** Chromium's fake camera, a real
  canvas, a real transfer, the real WASM build: a burst produces five candidates with distinct
  frame ids and non-zero dimensions. It is the only check that would notice the port going back
  to refusing, which — given this claim has now been wrong in five files across two reviews — is
  the point of writing it.
- **A grabbed frame is downscaled**, and every frame in the store is uncompressed RGBA. Both are
  temporary and both are recorded above rather than left to be discovered.
- **`SetLocks` still refuses**, so the client arms bursts with every lock off. That is honest and
  it is not free: candidates within a burst may differ in exposure, so comparing them on sharpness
  means less than it should. ADR 0019 records the underlying question — an asynchronous
  `applyConstraints` behind a synchronous port — as unresolved, and it is now the thing standing
  between this and a burst worth selecting from.
- **The layer rule is looser by one edge**, and the checker's tests pin both halves: a port may
  reach the frame store, and reaching any *other* port is still a violation.
- The page-to-worker protocol gains its fourth push. ADR 0019 said that past a handful of messages
  it wants generating; this is the message that makes "a handful" the right word.

## Rejected alternative
**The composition root allocates, and the port hands back what it was given.** `bridge/runtime.*`
is exempt from the layer rules by name (ADR 0014), so the copy could have happened there and
`BrowserCameraAccess` would have held a ready-made `FrameRef` with no frame-store dependency at
all. It was rejected because it only works if exactly one frame is pushed per peek, and nothing on
the page knows how many peeks the manager will make — the contract requires a *distinct* frame per
call, and the page cannot count them. Routing an allocation through the composition root to dodge
a rule that the native fake already breaks would also have been an exception in fact while
claiming not to be one in the checker.

**A seam inside the camera component, as ADR 0020 did for spill.** An `IFrameSink` the composition
root implements over the store would satisfy the letter of the rule. It was rejected because
ADR 0020's argument does not carry: there the destination genuinely varied — RAM natively, OPFS in
a browser — and the seam named a real axis. Here every implementation wants the same thing, the
frame store, and an interface with one implementation that exists to avoid an include is a fig
leaf rather than a boundary.
