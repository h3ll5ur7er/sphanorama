# ADR 0019 — The core moves to the worker it was always specified to run in

**Status:** accepted

## Context
This is less a new decision than an old one being paid for. [04 §4.1](../04-runtime-topology.md)
has listed a **core worker** — "WASM module: all managers, engines, adapter ports" — since the
architecture was written, for the reason given there: business logic runs off the UI thread by
construction. Phase 0 loaded the module in the page instead. That was a shortcut, taken to get a
walking skeleton onto a phone, and it was never written down as a decision, which is how a
shortcut becomes the design by default.

Two things now make it cost something rather than merely differ from the drawing.

**The frame store cannot spill from the main thread.** `Pin` faults a spilled frame back in, and
an engine calls it because it is about to read the bytes — there is no tick to spread that over
and no way to hand the caller a promise. It has to be synchronous, and OPFS is asynchronous
everywhere except `createSyncAccessHandle`, which is worker-only. Measured in Chromium:

| | main thread | worker |
| --- | --- | --- |
| `createSyncAccessHandle` | **not present** | present; the handle opens once in 0.6 ms |
| 8 MB write / read | 44 ms / 15 ms, async | 30 ms / 14 ms, synchronous |

Opening once and using it synchronously afterwards is exactly the shape ADR 0014 already
established. The obstacle was never the pattern; it was the thread.

**And the crossing turns out to be cheap.** A bare `postMessage` round trip is ~63 µs and a facade
call through one ~71 µs, against ~0.4 µs for the direct call the client makes today. The ratio is
alarming and is not the ratio that matters: one tick of the capture loop would spend 71 µs of a
16,667 µs frame. Against that, a 30 ms spill on the main thread costs about two dropped frames
every time it happens.

One correction worth recording, because it nearly decided this the wrong way. A worker looked at
first like it would cost iOS, on the grounds that getting camera frames into one means
`MediaStreamTrackProcessor` and Safari does not have it. It is one route and not the only one: the
page can grab a frame and hand the buffer over by transfer, which moves ownership rather than
copying. That round trip measures the same ~66 µs at 8 MB as at 1 MB, which is what zero-copy
looks like and is the check that the number means what it says.

## Decision
**The core moves into a dedicated module worker**, as 04 §4.1 always had it.

ADR 0014's rule is unchanged — ports are synchronous over resident state — but its wording is now
wrong in a way that matters. It says the *page-side* host keeps the data resident, because when it
was written the core was in the page and there was nowhere else to mean. Resident has to mean
**resident in the worker**: same address space as the caller, or the read cannot be synchronous.
Keeping it there is the page's job.

Where each port lands follows from what the platform allows, not from taste:

- **The project store moves wholesale.** IndexedDB is available in workers, so the document host
  goes across unchanged and ADR 0014 applies to it exactly as written.
- **The camera and motion adapters stay in the page** for now, and push. `getUserMedia` and a
  `<video>` element are page things, and `deviceorientation` is a window event. The page grabs the
  latest frame and transfers the buffer; the worker holds it and `PeekPreviewFrame` reads it. The
  page posts IMU batches; `Drain` reads them.
- **Spill is a new port over an OPFS sync access handle**, opened during worker startup and held
  for the session.

That last placement is a simplification of 04 §4.1's **capture worker**, which puts the camera and
motion adapters in a worker of their own doing `VideoFrame` → heap copies. That topology is still
the target and is strictly better — it keeps frame acquisition off the UI thread too — but it
needs `MediaStreamTrackProcessor`, so it is a Chromium-only optimisation rather than the way in.
Starting with the page pushing means one new context instead of two, and it works on every browser
we ship to. 04 §4.1 is updated to say so rather than being left describing an end state as though
it were the plan.

Calls that only write — `StartPreview`, `SetLocks`, `Close` — are posted and return `Ok` without
waiting, the same trade ADR 0014 already took for a document write and for the same reason.

None of this is threads in the WASM sense. A worker is not `SharedArrayBuffer` and needs no
cross-origin isolation, so ADR 0011 stands untouched and Pages still serves the single-threaded
build. Pixels cross by transfer, which 04 §4.1 already names as the supported path when the app is
not isolated.

## Consequences
- A facade call costs ~71 µs instead of ~0.4 µs — 0.4% of a frame, 4.3 ms of CPU per second at
  capture rate. The client's proxies are already `async`, so no client code changes shape.
- A 30 ms spill stops landing on the frame the user is watching. This is the reason to do it, and
  it gets better as frames get bigger, which they will.
- **There is now a page-to-worker protocol, and it is hand-written.** The facade crossing is thin —
  a method id and a byte array in, a byte array out, knowing nothing about method names — so it
  cannot drift. The host protocol can: capabilities, the latest frame, IMU batches. That is a new
  place for two sides to disagree, of exactly the kind ADR 0009 built a generator to prevent for
  the other crossing. Past a handful of messages it wants generating too.
- Startup gains steps that can fail independently: the worker boots, the module loads inside it,
  the OPFS handle opens. "The core failed to load" stops being one outcome and becomes three, and
  the client has to say which.
- Debugging moves into the worker context, which is worse. The end-to-end suite has to drive the
  page rather than reaching into the module — which is an improvement, since that is what a user
  does.
- Nothing crosses a contract differently. `ICameraAccess`, `IMotionSensorAccess`,
  `IFrameStoreAccess` and every manager interface are untouched, and nothing regenerates.
- **The iOS risk is real and unverified.** Nothing here needs an API Safari lacks, and every
  measurement above is Chromium's. Whether iOS Safari's sync access handles behave the same has to
  be checked on a device before this is load-bearing. If they do not, the fallback is a store that
  refuses to spill and a sphere capped at what fits in RAM — degraded, not broken.

## Rejected alternative
**Stay in the page and Asyncify the spill path.** ADR 0018 rejected Asyncify for the burst and
every reason still holds: the generated facade makes instrumentation all-or-nothing, and two
overlapping suspends crash the renderer. It is worse here. Even working perfectly, faulting a
frame in would still block the main thread for the length of an OPFS read — suspension changes
*who* waits, not *where* the work happens, and the frame is dropped either way.

Two others. **Queue spill writes and keep the core in the page**: `Demote` could be posted and
forgotten, but `Pin` cannot, and a store that spills without faulting back in is not a store.
**Never spill, and cap a sphere at what fits in RAM**: honest, and it would work today. It gives up
the candidate pool the retake feature is built on, which is the tens-of-gigabytes problem this
project is arranged around in the first place.
