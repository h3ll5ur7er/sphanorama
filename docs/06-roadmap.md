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
ratio** and an **assumed 66° angle across the frame's long edge** (`deriveFieldOfView` in
`shell/src/access/capture-host.ts`). Across the *long* edge rather than the horizontal one,
because the assumption is about a lens and a lens does not change when the phone is turned — the
browser reports the track in the device's current orientation, so a phone held upright answers
960×1280 and the wide angle belongs to its height. Phase 2's bundle adjustment estimates focal length from the
captured frames, which is the only way to actually know; until then a wrong assumption shows up as
cells that overlap more or less than intended rather than as a failure.

That first half was itself overstated until recently, and the correction is worth recording
because it is the shape of mistake this project is most likely to repeat: the page opened the
camera without asking for a resolution, so "the camera's real resolution" was the *browser's
default* — 640×480 in Chromium, measured, against a grabber that budgets for 1280 on the long
edge. Every frame the core had ever scored or stored was a quarter of the pixels the memory
ceiling was sized for, and the cap that exists to bound a burst was bounding nothing. The page now
asks for the long edge the grabber keeps, so the frame the core stores is the frame the camera was
opened to produce — and for a 4:3 shape, which is not a preference between two crops. A phone's
sensor is 4:3 and its widescreen video mode is made by discarding the top and bottom, so the taller
frame is *more* of the picture rather than a differently shaped piece of it. It is worth asking for
because vertical field of view sets the ring count: 66° across is 40° tall at 16:9 against 52° at
4:3, which is 44 planned cells rather than 32 — a third more sphere to shoot, from a frame that
sees less of it. All four numbers are measured through the running app rather than derived.

Orientation samples go through the facade to `CaptureSessionManager`, which asks `PoseEngine` for
an attitude and `CoveragePlannerEngine` for the nearest cell, and the reticle on screen is that
answer coming back. Nothing about coverage, acceptance or pose is decided
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

   **And the policy above it is in: the session cools a cell on every way out of a burst** (ADR
   0023) — completion, a failed score or rank, a retake, the end of a session. `CaptureSessionManager`
   offers the sink every candidate its own bursts produced, because a finished cell is not read
   again until the build or the review client asks — both of which go through `Pin` and fault it
   back in. So a sphere larger than the store is capturable, and peak
   heap is one burst plus what a retake faults in to score against. `Allocate` still refuses at
   the ceiling rather than evicting, which is now the backstop it should always have been rather
   than the thing a normal capture runs into first. Two limits are named in the ADR rather than
   left to be found: `OfferFrame`'s frames belong to the caller and are not cooled, so a session
   driven entirely through it — the bench — is bounded by its ceiling; and a review client
   faulting a sphere back in to display it has no natural "finished" moment, which is the caller
   that would justify eviction inside the store if one is ever needed.

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
3. **A pose engine worth the name.** `OrientationPoseEngine` now fuses rather than chooses: on a
   sample carrying both an attitude and a rate it predicts forward with the gyroscope, corrects
   part of the way to the reading, and charges the rest to the gyroscope's zero offset — which it
   therefore learns without a stillness detector, and keeps in `PoseState` because an engine is
   stateless per session (ADR 0024). A 0.02 rad/s offset used to put 1.15° of yaw into one second
   of dead reckoning from a device that never moved; it is now under 0.2°, and a reading 3° out on
   every sample comes out under 1.5°.

   **And the rates are real now** (ADR 0025). The browser adapter listens for
   `DeviceMotionEvent.rotationRate`, converts it into the viewfinder's frame — inverting the
   screen rotation, which is invisible in portrait and wrong by 90° in landscape — and attaches it
   to the attitude samples that follow, dropping anything older than 200 ms or missing an axis.
   iOS gates motion separately from orientation and a denial there is not a failed start.

   What decides whether a rate is real is the sample rather than the capability: `ImuSample` gains
   `hasAngularVelocity`, the same distinction `hasOrientation` already draws and for the same
   reason. That also fixed `Stability`, which used to tell the two apart by `hasOrientation` and
   so reported a phone swung between two matching attitudes as perfectly still while the
   gyroscope in the same sample read 3 rad/s.

   What is left on this line is a device. Every number here is from tests, and the gains — 0.1 s
   to correct, 0.5 s to learn an offset — have never met a real phone.
4. Deferred with reasons, not forgotten: the trimmed OpenCV WASM build (nothing needs it until
   Phase 2 registration, and the size budget has 8.36 MB of headroom), the `bench/` CLI, and the
   synthetic-dataset generator — both of which Phase 1's accuracy harness is the first thing to
   actually need.

---

## Phase 1 — Guided capture with bursts
*Goal: the capture experience, storing real data. Still no stitching.*

- `CoveragePlannerEngine`: ring/geodesic tessellation from lens FoV, acceptance cones, coverage
  and hole evaluation.
- `PoseEngine`: complementary fusion, gyro bias handling and stability gating are **done** (ADR
  0024, ADR 0025). The browser adapts `DeviceMotionEvent.rotationRate`, each sample says whether
  its rate was measured, and `Stability` judges each interval by the better signal available for
  it. What is left on this line is a device: the correction and offset time constants have never
  met one.
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
- Review Client v1: the sphere coverage map and per-cell candidate strip are **done**, and a pick
  is recorded through `ProjectManager.SetSelection` (UC-3). Two halves of it are not: the strip
  shows what the core knows about a candidate rather than the frame itself, because reading pixels
  back *out* of the store has no path across the worker — `PeekPreviewFrame` only goes inward
  (ADR 0021); and a recorded selection cannot be read back, because `SetSelection` writes one and
  no contract returns it, so an override lives in the client's memory and goes with the tab.
  Neither is hard; both are contract-shaped rather than code-shaped.

**Exit:** a full 360×180 capture on a mid-range Android and an iPhone completes without an OOM,
survives a tab reload and resumes, and every cell holds a scored burst. Measured peak memory
recorded per device class.

*Where this stands:* two of the three conditions are met on one device. A Pixel 9 Pro XL captured a
full sphere — 28 of 28 cells, every one holding a scored burst, no OOM — through the deployed
build. The reload-and-resume condition is met in a real browser — an end-to-end test drives it —
but not yet demonstrated on a phone. What is left, and what has landed since:

- **Reload and resume.** **Done**, and wired to a button. The core reads a session document
  written at every committed cell, replans from the spec and lens that document carries, and hands
  the frames it names back to the store through `IFrameStoreAccess::Adopt`, so a restored candidate
  can still be pinned (ADR 0029). Underneath it the OPFS tier survives a reload too: a fixed
  preferred name and a sibling index carrying the frame-to-offset map, with a tier of its own for
  any session that cannot take that pair, which is the property ADR 0020 added (ADR 0030). `Begin`
  empties the tier and `Resume` does not (ADR 0034), so a new capture no longer issues identities
  the tier is already holding frames under, and abandoned spheres stop accumulating on disk. And
  the page now knows: `ProjectSummary` carries `hasSession`, filled from the project's session
  document by the listing the page already makes at load, so a capture that was interrupted is
  offered back rather than discovered by attempting one — a successful `Resume` opens the camera,
  so probing would have started a capture nobody asked for (ADR 0036). A resume the core refuses
  says why and leaves a new capture one press away. An end-to-end test captures a cell in a real
  browser, reloads the tab, presses resume and finds the cell and its five candidates still there.
  Scope, decided with the maintainer rather than assumed: in practice there is never more than one
  unfinished sphere, because resuming means standing in the same spot again — so the page offers
  the newest project that has a session, and only that one. The case worth building for is
  "a call came in mid-capture", not "come back to it tomorrow" — so no `navigator.storage.persist()`,
  and the tier is cleared when a *new* session begins rather than at worker startup, which is what
  lets a reload find its frames still there.
- **A tier generation, so another project's document cannot outlive its pixels.** The gap ADR 0034
  leaves open, and it is narrow but silent. `Begin` empties the tier and the new capture reissues
  identities from 1, so a session document belonging to a *different* project still names frames
  that now hold someone else's pixels — and the fault-in cannot catch it, because the bytes are
  really there and really the right length. The same-project case is closed, since `Begin`'s
  `Checkpoint` replaces that project's document in the same call; this is the two-unfinished-spheres
  case the scope note above says does not arise in practice. The fix is an epoch the tier bumps on
  every clear, recorded in the session document and checked by `Resume`. It wants an ADR: where the
  generation lives, and whether a stale document is refused or offered as coverage without pixels.
- **A cap on candidates per cell.** Found on the iPhone, and the numbers are exact. Motion was
  unavailable there, so guidance never advanced and five bursts landed on one cell: 25 candidates
  of 1280×960×4 = 123 MB, against a ceiling of 128 MB — Safari does not report
  `navigator.deviceMemory`, so the store takes the stated fallback (ADR 0023). Ranking is what
  tips it over, because scoring a cell reads every candidate's pixels and faults the whole
  accumulated set back into the heap at once. Cooling had done its job; the set simply came back.

  The Pixel never reached it because each cell got exactly one burst. It is reachable there too,
  by retaking a cell enough times. The fix is a policy question rather than a bug — keep the best
  N after each ranking, or rank incrementally — so it wants deciding rather than patching, and it
  needs an ADR either way.
- **Peak memory per device class.** Never measured. The frame store's ceiling is probed from
  `navigator.deviceMemory` (ADR 0023) but nothing records what a real capture actually costs.
- **How long a camera takes to settle after a lock.** Also never measured, and now a number the
  code depends on: `BurstSpec::settleMs` defaults to 150 ms because one frame 16 ms after arming
  was unusable and one 96 ms after arming was not, on one device in one scene (ADR 0032). Too long
  only costs time; too short leaves a soft frame that still scores and still ranks, which is how
  this went unnoticed in the first place. It wants the same per-device-class treatment as the
  memory figure — a burst armed at a known target with the settle swept, and the sharpness curve
  read off it.
- **An iPhone.** *Run.* iOS 18 Safari, 1280×960, 32 cells planned. Two things worked that were
  not certain to: the OPFS spill tier opened (no "no spill tier" in the capabilities line, so
  Safari's synchronous access handles are there), and the white balance lock took — the only one
  of the three that camera offers. Two did not, and both are fixed or filed below. The exit
  criterion also wants a completed sphere and a peak-memory number from it, and neither exists
  yet on that device.

Two known gaps in the phase's own list, both contract-shaped: the review strip shows what the core
knows about a candidate rather than the frame itself, and a recorded selection cannot be read back.

*Answered.* A cell's five candidates scored 1186, 1180, 459, 459, 458 in capture order, and
another 979, 993, 0.60, 0.60, 0.61 — both splitting two-and-three at the same point, in different
scenes, tight inside each group. The `locks` row (ADR 0022) settled it on the first device
reading, and the answer was neither of the two the question anticipated: `focus · exposure
refused · white balance refused`. The lens advertised a manual exposure mode and then would not
take it, so the burst fired with auto-exposure free for the whole third of a second. The strip
confirms it from the other side — the sharpness cliff falls on exactly the frames whose exposure
agreement drops (1.00, 1.00, 0.67, 0.78, 0.84), climbing back as the metering settles.

So not bracketing, and not the selection policy: a lock that was asked for and did not take. The
negotiation is fixed — one constraint set per lock rather than all three in one, an exposure time
offered alongside `manual`, and `single-shot` as a fallback (ADR 0031). Whether it
now holds on that camera is the next thing a screenshot answers.

And if it still does not, the same screenshot now says what the camera claims to offer: a refusal
is written against the mode list the track reported, so `exposure refused (offers continuous,
manual)` — a camera contradicting itself — is legible from `exposure refused (offers continuous)`,
which is a camera with no lock to give, and from `exposure refused (not reported)`, which is a
browser that would not answer. Reported only, never used to decide what to ask for: browsers
under-report, and the iPhone's one working lock cannot be told apart today from a lock that camera
was simply already sitting in — which is the same collapse, and the second thing that screenshot
now answers (ADR 0033).

*And the next reading found the other half of it.* With a focus lock actually held, a five-frame
burst on the Pixel scored 5.9, 1145, 720, 583, 586 — four frames within about 2× of each other and
a first one a hundredth of any of them. The iPhone, which takes a white balance lock and no focus
lock at all, has no such first frame. The lock is the cause rather than the scene: applying a focus
mode makes the camera re-converge, and `PeekPreviewFrame` borrows the *latest* preview frame, so
the frame taken 16 ms after arming is one from mid-refocus — or one the camera produced before the
constraints landed. A fifth of every burst was going in the bin on the device that succeeds at
locking, invisibly, because the bad frame is a real candidate with a real score that ranking simply
never picks. `BurstSpec` now carries a `settleMs` the first frame waits out (ADR 0032).

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
