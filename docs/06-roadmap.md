# 6. Delivery plan

Each phase has a demo and an exit criterion. Nothing is "done" because code exists; it is done when
the criterion is measurable. Phases are ordered so that the riskiest unknowns (memory ceilings,
sensor quality, iOS behaviour) are hit early, not at the end.

---

## Phase 0 — Skeleton and toolchain
*Goal: a build that proves the boundary works, containing no algorithms at all.*

What this phase set out to deliver, and what it actually delivered where the two differ — the
differences are decisions, each with an ADR:

- Repo layout, CMake presets, pinned emsdk, Vite PWA shell. *The trimmed OpenCV WASM build was
  deferred: nothing needs it until Phase 2 registration, and carrying it would have meant tuning a
  build for code that does not exist.*
- Utilities bar: `Result<T>`, status codes, logger, clock, config, arena.
- The generated boundary: the C++ header **is** the IDL (ADR 0009), generating the TypeScript
  mirror, both halves of a **binary wire codec** (ADR 0013 — not FlatBuffers, which would have
  meant a second schema language and toolchain) and the facade dispatch, over a **C ABI** rather
  than Embind (ADR 0012).
- All three managers, and engines that turned out to need real implementations sooner than
  planned: `CoveragePlannerEngine` tessellates for real, and `OrientationPoseEngine` folds the
  browser's fused attitude (ADR 0015) — a null planner cannot place a reticle, which is the exit
  criterion. `FrameQuality`, `Registration` and `Composition` are still null.
- Real `ICameraAccess` and `IMotionSensorAccess` adapters, plus the port mechanism behind them
  (ADR 0014). *`CaptureBurst` refused: it was the one call that could not be made resident in
  advance. The measurements were taken and it left the contract — a burst is paced by the manager
  over `PeekPreviewFrame` now (ADR 0018).*
- The test machinery the rest of the plan depends on: GoogleTest harness and the resource-access
  fakes behind shared contract suites (ADR 0010). *The synthetic-dataset generator and the frame
  folder / recorded IMU log the fakes would replay are deferred to Phase 1, which is the first
  thing that needs them.*
- CI: layer check, contract-drift check, no-browser native build, size budget — plus a test suite
  for each checker, since a checker that passes everything reads as a green light.

**Exit:** a phone opens the PWA, sees a live viewfinder with a reticle whose position is driven by
real sensor data routed *through the WASM core*, and the whole round trip stays under budget.
The core binary is under 8 MB and the same core compiles as the native bench.

*Where this stands:* the exit criterion is met. The PWA loads the WASM core, opens the camera,
tells the core what it got, and the core plans a real tessellation for it — 32 cells across 7
rings for a typical phone.

One caveat worth stating plainly, because "planned for your lens" overstates it: the browser does
not report field of view at all. The plan is sized from the camera's **real resolution and aspect
ratio** and an **assumed 66° horizontal angle** (`deriveFieldOfView` in
`shell/src/access/capture-host.ts`). Phase 2's bundle adjustment estimates focal length from the
captured frames, which is the only way to actually know; until then a wrong assumption shows up as
cells that overlap more or less than intended rather than as a failure. Orientation samples go through the facade to `CaptureSessionManager`,
which asks `PoseEngine` for an attitude and `CoveragePlannerEngine` for the nearest cell, and the
reticle on screen is that answer coming back. Nothing about coverage, acceptance or pose is decided
in the client.

The generated facade is in: the client calls managers through typed proxies, and a domain failure
arrives as a `Status` it can branch on. The ports are in too — the project store persists through a
reload (ADR 0014), and the camera and motion ports read state the page established before the
session began.

What is left before Phase 1 can start in earnest, in the order it blocks:

1. **A path for pixels.** Decided, with the measurements ADR 0014 asked for: the burst is paced by
   the manager across the ticks the client already makes, over a preview frame the page keeps
   resident, and `CaptureBurst` leaves the contract (ADR 0018). Asyncify turned out to be cheap —
   +3.2 kB gzipped and +30 ns on a facade call — and was rejected anyway, because the generated
   facade makes its instrumentation all-or-nothing and the one-suspend-at-a-time rule it imposes
   is enforced by crashing the renderer. What is left is the reshape itself: `CaptureCell` arms a
   burst instead of returning one.
2. **`IFrameStoreAccess` with real residency**, which is what those frames need somewhere to go —
   and, since a paced burst takes one frame per tick, an allocation per tick rather than a
   burst-sized batch. `MemoryFrameStoreAccess` is the first real implementation and the one the
   native build and the bench use: a stated ceiling it refuses to overrun, residency tiers, and a
   fault-in on `Pin` that re-checks the ceiling. Its spill tier is still RAM, which is honest on a
   desktop and a lie on a phone, so the WASM build stays on the null store. Two things are left,
   and neither is a detail: **the browser store over OPFS**, and **the ceiling probe** — the
   contract says that number is measured at startup and it is currently assumed.

   The browser store has a constraint worth knowing before it is designed, because it decides
   where the core runs. `Pin` faulting a spilled frame back in has to be synchronous — an engine
   asks for bytes and reads them — and OPFS is asynchronous except through
   `createSyncAccessHandle`, which is **worker-only**. Measured in Chromium: on the main thread,
   where the core runs today, that method is not even present; in a worker it is, the handle
   opens once in 0.6 ms, and reads and writes are then synchronous and roughly twice as fast as
   the main-thread async path (8 MB: 30 ms write / 14 ms read, against 44 ms / 15 ms). Opening
   once and holding it is precisely the resident-host shape ADR 0014 already uses for the project
   store, so a worker would make spill an ordinary synchronous port with no Asyncify and no
   contract change — and would also keep a 30 ms spill off the frame the capture loop is drawing.
   What it costs is the camera: the ports read a host that lives in the page, and getting camera
   frames into a worker means `MediaStreamTrackProcessor`, which Safari does not have. That
   trade looked at first like a worker for the frame store against a page for the camera. It is
   not: `MediaStreamTrackProcessor` is one way to get pixels into a worker and not the only one,
   and the page can grab a frame and hand the buffer over by transfer, which is zero-copy and
   needs nothing Safari lacks. The camera can stay in the page.

   What a worker actually costs is the round trip. Measured in Chromium, with the core booted in
   a module worker and both paths timed in one page: a bare `postMessage` round trip is ~63 µs
   and a facade call through one ~71 µs, against ~0.4 µs for the direct call the client makes
   today. Transferring a frame-sized buffer costs the same ~66 µs at 1 MB and at 8 MB, which is
   the zero-copy result and the sanity check on the number. So one tick of the capture loop would
   spend 71 µs of a 16,667 µs frame — 0.4% — to move a 30 ms spill off the frame the user is
   looking at, where it currently costs about two dropped ones. Numbers are Chromium on a loaded
   machine; a phone will differ, and there are two orders of magnitude of headroom for it to.

   Decided in ADR [0019](adr/0019-the-core-runs-in-a-worker.md), and **done**: the core runs in
   the worker [04 §4.1](04-runtime-topology.md) always specified and Phase 0 shortcut, with the
   document host across with it and the camera and motion halves fed from the page. What is left
   for the browser store is the OPFS sink itself — the handle opened at worker startup, `Demote`
   writing through it, `Pin` faulting back in — plus the ceiling probe, which is the other thing
   still assumed.
3. **A pose engine worth the name.** `OrientationPoseEngine` prefers the browser's fused attitude
   and integrates rates when there is none (ADR 0015). That is enough to aim; it is not
   complementary fusion, and gyro bias is not handled at all.
4. Deferred with reasons, not forgotten: the trimmed OpenCV WASM build (nothing needs it until
   Phase 2 registration, and the size budget has 8.36 MB of headroom), the `bench/` CLI, and the
   synthetic-dataset generator — both of which Phase 1's accuracy harness is the first thing to
   actually need.

---

## Phase 1 — Guided capture with bursts
*Goal: the capture experience, storing real data. Still no stitching.*

- `CoveragePlannerEngine`: ring/geodesic tessellation from lens FoV, acceptance cones, coverage
  and hole evaluation.
- `PoseEngine`: complementary/Madgwick fusion, gyro bias handling, stability gating.
- `CaptureSessionManager`: full reticle → hold-still → burst → accept loop; per-cell candidate sets.
- `FrameQualityEngine` v1: sharpness (variance of Laplacian on a downscale), exposure agreement,
  motion-blur proxy from angular velocity.
- `IFrameStoreAccess` with the tiered residency and OPFS spill; memory-budget probe.
- Review Client v1: the sphere coverage map, per-cell candidate strip, manual selection.

**Exit:** a full 360×180 capture on a mid-range Android and an iPhone completes without an OOM,
survives a tab reload and resumes, and every cell holds a scored burst. Measured peak memory
recorded per device class.

---

## Phase 2 — Stitching
*Goal: a real panorama out the other end.*

- `RegistrationEngine`: ORB/AKAZE extraction, ratio-test + geometric matching, sensor-prior-seeded
  pure-rotation estimation with RANSAC, then a global bundle adjustment over rotations and shared
  intrinsics (focal + radial distortion).
- `CompositionEngine`: gain/vignette exposure compensation, graph-cut seam finding, multi-band
  blending, equirectangular projection with tiled output.
- `PanoramaBuildManager`: staged progress, low-res preview first, then full render.
- `ProjectManager` export: JPEG/AVIF with XMP `GPano`.
- Accuracy harness on synthetic datasets wired into CI.

**Exit:** synthetic-dataset registration median error under a stated angular threshold; a real
capture exports a file that Google Photos and a WebXR viewer open as a sphere; end-to-end build
time recorded per device class.

---

## Phase 3 — The differentiators
*Goal: the reasons this exists.*

- Build graph with fingerprinting and `Invalidate(dirtyNodes)` incremental rebuild.
- Retake flow end to end: flag → re-arm reticle → recapture cell → partial rebuild.
- Ghost detection from intra-cell candidate disagreement and inter-cell overlap; ghost report
  surfaced on the sphere; mover-aware seam costs.
- `FrameQualityEngine` v2: mover penalty, inter-candidate alignment residual, user override.

**Exit:** a scene shot with a person walking through it produces a visibly ghosted region, the
region is highlighted automatically, and a retake of the affected cells clears it with a rebuild
measurably faster than a full one (target: an order of magnitude).

---

## Phase 4 — Speed and reach
*Goal: it feels like a camera, on the phones people actually have.*

- `IComputeDeviceAccess` WebGPU backend for warping, blending and pyramid construction, with the
  CPU path retained as the correctness reference (a differential test asserts they agree).
- Threaded feature extraction and blending; single-threaded path verified in CI.
- PWA polish: installable, fully offline, share-target export, background-safe builds.
- Degraded modes: no-sensor capture, no-SAB capture, low-memory device profile.

**Exit:** a stated build-time target met on a mid-range device with WebGPU, the CPU path within a
stated factor of it, and the app fully functional offline after first load.

---

## Phase 5 — Beyond parity
Candidates, in the order they fit the existing seams:

- HDR/bracketed bursts (a `FrameQualityEngine` + `CompositionEngine` strategy, no new components).
- Cubemap output and a built-in WebXR viewer.
- Continuous "sweep" capture as an alternative `CaptureSessionManager` policy.
- Depth-aware de-parallax using the multi-view geometry the burst already gives us.

---

## What to build first, concretely

The single highest-value first commit after this architecture is **Phase 0's boundary**: the IDL,
the generated facade, and a null core that a phone can drive with real sensor data. It is the piece
every later phase depends on, it is where the platform surprises live (cross-origin isolation, iOS
permissions, heap ceilings), and it is worthless to discover any of that in Phase 3.
