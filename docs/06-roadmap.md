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
  criterion. `FrameQuality` followed once a burst had real frames to judge; `Registration` and
  `Composition` are still null.
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
   is enforced by crashing the renderer. The reshape landed with it: `CaptureBurst` is out of the
   contract, `CaptureCell` became `ArmBurst`, and `OnMotion` takes one frame per tick.
2. **`IFrameStoreAccess` with real residency**, which is what those frames need somewhere to go —
   and, since a paced burst takes one frame per tick, an allocation per tick rather than a
   burst-sized batch. `MemoryFrameStoreAccess` is the implementation both platforms use: a stated
   ceiling it refuses to overrun, residency tiers, and a fault-in on `Pin` that re-checks it.
   Where a spilled frame's bytes go is a seam inside it (ADR 0020) — an OPFS sync access handle in
   the browser, opened once by the worker and held for the session, and nothing natively, where a
   store with no sink refuses to spill rather than relabelling a frame it has not moved. So the
   WASM build is off the null store and *can* spill.

   **The browser's ceiling is read from the device now**: a sixteenth of `navigator.deviceMemory`,
   clamped by three quarters of what the module was linked to allow and floored where one burst
   stops fitting. So a 1 GB phone and an 8 GB desktop no longer get the same number, which is what
   "measured at startup, not assumed" was asking for. Two limits are worth knowing rather than
   discovering: Safari and Firefox do not report `deviceMemory` at all, so every iPhone takes the
   stated fallback and the improvement is Chromium's; and this reads the device rather than
   confirming what the tab will actually be allowed to keep. Nothing in the platform answers that,
   and the obvious probe — allocate and see — is ruled out twice over, because WASM heap growth is
   one-way and because pushing to the allocator's refusal is walking up to the cliff the ceiling
   exists to stay away from.

   **What is left is the policy: nothing decides when to spill.** `Allocate` refuses at the
   ceiling instead of evicting, and no production caller demotes, so the tier is a finished
   mechanism with a real number under it and nothing above it.

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
   document host across with it and the motion and camera-capability halves fed from the page. The
   sink followed — the handle opens at worker startup, `Demote` writes through it and `Pin` faults
   back in — and it turned out not to need a store of its own at all (ADR 0020).

   **Pixels cross now** (ADR 0021), which was the next thing and is done: the page draws the
   viewfinder into a canvas, transfers the buffer, the worker holds it, and `PeekPreviewFrame`
   allocates a frame in the store and copies it in. Arming moved into the capture loop at the same
   time, closing the hole ADR 0018 named, and an end-to-end test drives a real burst in a real
   browser and checks it produced five candidates with distinct frames.

   **The locks followed** (ADR 0022), which was the thing standing between a burst that captures
   and a burst worth selecting from. The page applies them with `applyConstraints` and reads the
   mode back to confirm — resolving is not applying — and pushes what the camera actually settled
   on; `SetLocks` reads that state and refuses a lock the page has not confirmed, so a burst
   cannot arm believing it holds one it does not. Releasing goes back the other way, posted, since
   nothing waits on a lock given up after the burst is over. A camera with no manual mode still
   captures and says which lock it could not take.

   One debt remains from the pixel path and it is written down rather than left to be found: a
   grabbed frame is **capped at 1280 on its long edge**, because RGBA is four bytes a pixel and
   nothing compresses anything yet. The frame that gets stitched should be the full-resolution
   one, and that means encoding to JPEG before it crosses.

   One thing the numbers above do not settle, and it is the load-bearing one: **every measurement
   here is Chromium's**, and the end-to-end suite proves the handle opens in headless Chromium and
   nothing more. Whether iOS Safari's sync access handles behave the same has to be checked on a
   device. If they do not, the sink simply does not install and the store refuses to spill at all
   — a sphere capped at what fits in RAM, which the capture client says out loud rather than
   discovering partway through.
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
- `FrameQualityEngine` v1: sharpness (variance of Laplacian on a downscale) and exposure
  agreement are **done** — `SharpnessFrameQualityEngine` is what the WASM build and the bench now
  use, and `Rank` normalises sharpness across the candidate set before weighting it, so every
  weight in `SelectionPolicy` changes an answer rather than only the sharpness one. The
  motion-blur proxy is **not**: turning an angular rate into pixels of smear needs the exposure
  time and the focal length in pixels, and the engine is handed neither. It reports zero and the
  header says so, because a number invented from what it does have would rank frames by a
  fiction. Both inputs exist elsewhere — the camera port could report exposure time, and Phase 2's
  bundle adjustment produces a real focal length — so this waits on one of them rather than on
  an idea.
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
