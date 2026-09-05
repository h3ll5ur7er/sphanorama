# ADR 0038 — A frame leaves the core reduced, and a new engine is what reduces it

**Status:** accepted

## Context
The review strip has been showing what the core *knows* about a candidate — a sharpness figure and
an exposure agreement — because that is all it could get. `ICaptureSessionManager::Candidates`
hands back `Candidate`s, each carrying a `FrameRef`, and a `FrameRef` is a handle into a frame
store the page does not have. `IFrameStoreAccess::Pin` is the only route to bytes and it reaches no
further than the core; the one pixel path that exists, `PeekPreviewFrame`, goes *inward*
([ADR 0021](0021-the-pixel-path-crosses-by-transfer-and-lands-in-the-frame-store.md)). The roadmap
named the gap in as many words and called it contract-shaped.

It is worth being precise about what a strip of numbers cannot do, because the numbers are real
and they are not useless. A burst exists so that a person can end up with the best of five frames
of the same wall. `Rank` says which one that is and the strip shows its reasoning — but the
question a reviewer actually has is *"is the thing I wanted even in the picture"*, and no
aggregate answers it. A frame can be the sharpest of its burst and be of the ceiling.

So pixels have to come out. The interesting question is how many of them.

## Decision

### The size, which decides everything else
**A frame leaves the core reduced, never whole.** The arithmetic is the argument:

| | one candidate | a cell of 5, the `BurstSpec` default | a cell of 8, the most a cap would leave |
| --- | --- | --- | --- |
| the stored frame, 1280×960 RGBA | 4.9 MB | 24.6 MB | 39.3 MB |
| reduced to a long edge of 256 | 192 KB | 960 KB | 1.5 MB |
| reduced to a long edge of 128, which the strip asks for | 48 KB | 240 KB | 384 KB |

Against a browser heap ceiling of 128 MB ([ADR 0023](0023-a-committed-cell-is-cooled-by-the-session.md)),
handing back whole frames means one cell of a strip costing a fifth to a third of everything the
device has — a retaken cell today can hold considerably more than eight —
and it costs it three times over, because the same bytes are simultaneously the faulted-in frame,
the encoded result inside `g_result`, and the copy the page holds. `g_result` is a `static
std::vector` that never gives its capacity back, so a single full-frame call would grow the core's
result buffer by 4.9 MB for the rest of the session.

At 48 KB none of that is true, and that is the whole point: **the rule that pixels never cross a
contract is a rule about cost, and the reduction is what pays it.** `contracts/README` and the
engineering skill both say "no pixels in a contract" and both are amended here rather than
excepted, because the sentence under the rule — frames cross as handles, `Pin` is the only route
to bytes — was never about a thumbnail. It was about not serialising megabytes by value. A handle
also cannot do the job at all in this direction: there is nothing on the page to resolve one
against.

`IImageCodecAccess::Encode` already returns `std::vector<uint8_t>` across a `@boundary` interface,
so bytes crossing outward is not new. What is new is that these bytes are an image of a frame.

### Where the reduction happens: a new engine, and a new axis
The layer rules make this the real question. A manager doing pixel arithmetic is the thing this
repo's layer discipline exists to prevent, and the four components that could plausibly own a
reduction each own something else:

- **`IFrameStoreAccess` (V11)** is *where pixel bytes live*. Residency, the ceiling, the tiers —
  none of that is resampling. It is also the contract held equal across implementations by a
  suite ([ADR 0010](0010-resource-access-contract-suites.md)), and "these two implementations
  resample identically" is exactly the kind of property a contract suite cannot usefully check —
  the same argument [ADR 0020](0020-the-spill-destination-is-a-seam-inside-the-frame-store.md)
  made about the ceiling.
- **`IFrameQualityEngine` (V6)** is *what "best" means*. A thumbnail is not a judgement, and V6 is
  the most-tuned component in the system — the last place to hang something that has to keep
  working while the selection policy is being rewritten. That it already downscales a luma plane
  is a coincidence of technique, not a shared responsibility.
- **`ICompositionEngine` (V8)** is *how pixels become one image*, and its axis does list
  resampling. But every method it has takes a `GlobalSolution`: it is many frames becoming one
  panorama, owned by `PanoramaBuildManager`. Putting a per-frame thumbnail there would draw an
  edge from `CaptureSessionManager` to the composition engine, which says on the service map that
  a capture session composes panoramas. It does not.
- **`IComputeDeviceAccess` (V14)** is *where heavy math executes*, and `Kernel::Downsample` is
  already in its enum. It has no implementation on either platform and is Phase 4 work; routing a
  strip through it would mean building a compute backend to draw a thumbnail, and it answers
  "where does this run", not "what does it produce".

None of them owns it, which under `docs/00-principles.md` §0.5 is real information rather than a
reason to force a fit. So the volatility map gains a row:

> **V16 — how a stored frame is made small enough to look at**: the reduction factor, the filter,
> and the pixel format it lands in. Varies over the surface doing the reviewing and over what the
> crossing costs. Encapsulated by `FramePreviewEngine`.

`IFramePreviewEngine::Reduce(frame, maxEdge)` returns a `FramePreview`, and
`BoxFramePreviewEngine` is the implementation: whole-block box averaging, the same choice and the
same reasoning as the sharpness measure's downscale — a resampler is a second thing to get wrong,
and averaging rather than sampling is what stops a textured wall aliasing into moiré and two
frames of it looking different from each other. Being an engine is what makes the algorithm
replaceable without touching a caller, which matters because the filter and the format are
precisely what this axis says will change.

### The shape at the boundary
`ICaptureSessionManager::CandidatePreview(node, candidate, maxEdge)`, beside `Candidates`. The
client calls managers only, and this is the same cell's same list one call further down.

- **One candidate per call, not a cell.** A strip fills in as its answers arrive, a cell the user
  has left stops costing anything at the next candidate rather than the last, and — the reason
  that decided it — a candidate whose frame has gone is one row saying so rather than a strip that
  failed. A replace-retake forgets a cell's frames, and `candidates.ts` already has the precedent
  of a selection outliving what it names.
- **RGBA8, tightly packed.** It goes straight into an `ImageData` and onto a canvas with no decode
  step anywhere. Encoding to JPEG would be about four times smaller and would need a decoder on a
  path that has none; at 48 KB there is nothing to buy.
- **`maxEdge` is the caller's, bounded by the contract.** How large a thumbnail wants to be is a
  fact about the screen it is going on, and the strip asks for 128 because its box is 4 rem on a
  2× display. Past `kFramePreviewMaxEdge` (256) the call is refused rather than clamped: a caller
  asking for a whole frame is asking for the thing this exists to avoid, and quietly handing back
  something else is how a budget stops being one.
- **Nothing new crosses between the page and the worker.** The pixel path inward needed a message
  of its own because it is a push; this is a call and an answer, which the facade already is — and
  the worker already transfers its result buffer rather than copying it. The one copy on this side
  is 48 KB into a `Uint8ClampedArray`, because `ImageData` will not take a buffer that might be
  shared and the threaded build's is ([ADR 0011](0011-single-threaded-build-for-github-pages.md)).

### Reading a preview does not warm a cell
This is the half that is easy to miss and would have been the expensive one. `Pin` faults a
spilled frame into the heap **and leaves it there** — that is exactly why
[ADR 0023](0023-a-committed-cell-is-cooled-by-the-session.md) cools a whole cell rather than one
burst. A strip that pinned eight frames to look at them would leave 39 MB resident, and a user who
opened three cells would have filled a phone's heap by browsing. ADR 0023 named this caller in
advance, in its rejected alternative: *"a review client faulting a sphere's frames back in to
display them is a caller with no natural finished moment"*.

It has one. The moment is when the reduced copy exists, and the session is what knows it — the
same split ADR 0023 drew: **the session owns the moment, the store owns the mechanism and the
refusal.** `CandidatePreview` reads the frame's residency before the reduction and restores it
after, on every path out including the failing ones, discarding the status for the reasons `Cool`
discards one. It restores what it *found* rather than imposing a tier, which is what keeps it safe
for a frame that arrived through `OfferFrame` and belongs to the caller.

## Consequences
- The strip shows the frames. One of Phase 1's two known gaps is closed; the other — a recorded
  selection that cannot be read back — is untouched and still contract-shaped.
- **`FramePreview` is the first byte payload to cross this boundary**, and the first field of any
  kind that is length-prefixed. Both suites now pin a golden hex for one, beside the guidance
  golden ADR 0013 introduced: a prefix width or endianness the two halves disagreed about would
  decode into a plausible image of the wrong size rather than into a failure.
- **The generator could not carry an `int32_t` parameter, and now can.** Every number crosses as a
  double, and the dispatcher declared its parameters from the *mirror's* type — so `maxEdge`
  arrived as a `double` and passing it to an `int32_t` method is a narrowing the core refuses to
  compile. Nothing had hit it because no `@boundary` method had ever taken an integer. A parameter
  now carries its C++ type alongside its TypeScript one, which is the sort of hole ADR 0009 said
  the strict subset would surface rather than hide.
- **An integer parameter is read checked, and an integer *field* still is not.**
  `static_cast<int32_t>(in.GetF64())` is undefined for a NaN, an infinity or a 1e300 — exactly what
  `GetId` was before `IsRepresentableId`. The parameter path this ADR opens gets the same
  treatment: `wire::GetInteger<T>` refuses a value the conversion is not defined for and fails the
  reader, so the facade answers "malformed arguments" instead of passing on whatever the cast
  produced.

  Fields get it too. That was not the first answer here — the review asked for the *wire kind* to
  change, and this ADR declined that for parameters alone, because every `int32_t` field had
  crossed as an f64 since ADR 0013 and making one C++ type cross two ways depending on its
  position would be worse than a lossy-but-uniform rule. The check is a different question from
  the format, and the distinction is what makes it cheap: the encode is untouched, so the golden
  hexes hold and a decoder from before this reads the same bytes. What changes is that a number
  that was never an integer fails the reader instead of quietly becoming one. `Field` now carries
  its C++ type for the same reason `Param` does — every width of integer maps onto `number`, so
  the mirror cannot say which one to check against.

  What is still filed rather than done is the format: `i32` on the wire for narrow integers,
  fields and parameters alike. It is smaller on the wire and it is no longer a correctness
  question, because nothing casts unchecked any more. It updates every generated struct in both
  languages and both golden hexes, which is a change of its own.
- **The review client caches an open cell's previews.** Recording a pick re-opens the cell, and
  asking for every preview again would fault each frame in and spill it out once more — roughly
  350 ms of file work for a cell of eight, to redraw pictures the page is still holding. The cache
  is dropped when another cell is opened, because holding a sphere's thumbnails would be the
  review client's own copy of the problem the reduction exists to solve.
- **A cell that is *not* spilled is read and left alone**, which on the bench and on any browser
  without an OPFS tier is every cell. The restore is a no-op there, as it should be.
- **The engine refuses `NV12` and `I420`.** Their luma plane is readable — the sharpness engine
  reads it — but a preview is a colour image, and producing one means a chroma upsample and a
  colour matrix that would be invented rather than measured. A grey preview of a colour frame is a
  wrong answer that looks like a right one. Nothing produces those formats today; `OfferFrame`
  could.
- **The reduction is not free and it is not cached in the core.** Opening a cell reads every
  candidate's full frame, which on a spilled cell is eight OPFS reads and eight writes. It is off
  the UI thread (ADR 0019) and it happens once per cell visit. If reviewing ever becomes something
  a user does at length, the answer is a preview kept beside the candidate rather than a bigger
  cache on the page — and that is a decision to take with a measurement, not in advance.
- **`FrameRef::width` is trusted nowhere.** The engine checks the handle's dimensions against the
  bytes `Pin` actually returned before reading a row of them, because a `FrameRef` is a plain value
  a caller passes in and the store's entry is the only thing that knows how large the allocation
  really is.

## Rejected alternatives

**Hand back the whole frame and let the page downscale it.** The page has a canvas and can resize
an image better than a box filter can; the core would need no new component and no new axis. It
was rejected on the arithmetic above — 39 MB across the wire for a cell, held simultaneously in
three places, on a device with a 128 MB ceiling — and on a second thing that is easy to miss: the
*reason* to reduce is transport cost, which is the core's business, not display size, which is the
page's. The page would be doing the core's arithmetic with none of the numbers.

**A `FrameRef` out and a second, hand-written pixel path back.** The manager could allocate a
thumbnail frame in the store and return its handle, and the page could then ask the worker for
those bytes over the hand-written page-to-worker protocol, reading them out of the module's heap
through a small C ABI export — which keeps "no pixels in a contract" literally true. Rejected: it
adds a second marshalling path the generated codec does not cover, of exactly the kind ADR 0012
collapsed into one, and it hands the page a handle to a frame it has no way to release. A client
cannot call `IFrameStoreAccess`, so every strip the user opened would leak a thumbnail until the
session ended.

**Encode to JPEG through `IImageCodecAccess` (V13).** The contract already exists, it is
`@boundary`, and it is the honest long-term home for "a frame leaves the core as an image" — the
browser would encode with its own codecs and the page would draw a blob. Rejected for now on
sequencing rather than on principle: V13 has no implementation on either platform and is Phase 2
work, it needs a decode on a page that has none, and it does not answer the size question anyway —
a JPEG of a full 1280×960 frame is still several hundred kilobytes, five times what a reduced RGBA
thumbnail costs. When V13 lands, the *format* behind `CandidatePreview` can change without the
manager's contract moving, which is the property that makes deferring it cheap.

**Produce the previews at capture time and keep them.** Every candidate's thumbnail could be made
while the burst's frames are already pinned for scoring, which would make a strip instant and cost
no fault-in at all. Rejected because they have to live somewhere: at 48 KB a 28-cell sphere of
five is 6.7 MB held for the whole session, in a manager, in a codebase whose rule is that pixels
live in the frame store. It also decides a thumbnail size before any client has asked for one, and
charges every capture for a review most of them will not get.

**A downscaling utility called from the manager.** The utilities bar carries real arithmetic
already — `quaternion.h` is not a triviality — so a `ReducePixels` free function would need no new
component and no new axis. Rejected because it launders rather than answers: the manager would
still be the thing pinning a frame and walking its pixels, and a utility is not swappable. What
this axis says will change is the filter and the format, and an engine is the shape that lets them
change behind a contract.
