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
  `Composition` were still null at the end of Phase 1, and `Registration` is partly real now — see
  Phase 2 below.
- Real `ICameraAccess` and `IMotionSensorAccess` adapters, plus the port mechanism behind them
  (ADR 0014). *`CaptureBurst` refused: it was the one call that could not be made resident in
  advance. The measurements were taken and it left the contract — a burst is paced by the manager
  over `PeekPreviewFrame` now (ADR 0018).*
- The test machinery the rest of the plan depends on: GoogleTest harness and the resource-access
  fakes behind shared contract suites (ADR 0010). *The frame folder and recorded IMU log the fakes
  would replay are still deferred. The synthetic-dataset generator arrived in Phase 2 rather than
  Phase 1 — `tools/synth_dataset.py`, ADR 0050 — because what first needed it was measuring
  registration accuracy, and Phase 1 does no registration.*
- CI: layer check, contract-drift check, no-browser native build, size budget — plus a test suite
  for each checker, since a checker that passes everything reads as a green light.

**Exit:** a phone opens the PWA, sees a live viewfinder with a reticle whose position is driven by
real sensor data routed *through the WASM core*, and the whole round trip stays under budget.
The core binary is under 8 MB. (This sentence also claimed the same core "compiles as the native
bench"; it does compile natively — that is what every test run does — but there is no `bench/`
client for it to compile *as*, and saying so implied one existed.)

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
4. Deferred with reasons, not forgotten: the trimmed OpenCV **WASM** build (nothing needs it in a
   browser until Phase 2 registration ships, and the size budget has 8.36 MB of headroom — the
   native build of the same trimmed subset is in, ADR 0047) and the `bench/` CLI, which Phase 2's
   accuracy harness is the first thing to actually need. (This said Phase 1's, from before the
   harness had a phase: it measures registration accuracy, and Phase 1 does no registration. The
   synthetic-dataset generator was listed here too until it was built — ADR 0050.)

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
  agreement are **done** — `SharpnessFrameQualityEngine` is what the WASM build and the native
  tests use (there is no bench yet, whatever the line above this one used to say), and `Rank`
  normalises sharpness across the candidate set before weighting it, so every
  weight in `SelectionPolicy` changes an answer rather than only the sharpness one. The
  motion-blur proxy is **not**: turning an angular rate into pixels of smear needs the exposure
  time and the focal length in pixels, and the engine is handed neither. It reports zero and the
  header says so, because a number invented from what it does have would rank frames by a
  fiction. Both inputs exist elsewhere — the camera port could report exposure time, and Phase 2's
  bundle adjustment produces a real focal length — so this waits on one of them rather than on
  an idea.
- `IFrameStoreAccess` with the tiered residency and OPFS spill; memory-budget probe.
- Review Client v1: the sphere coverage map and per-cell candidate strip are **done**, a pick is
  recorded through `ProjectManager.SetSelection` (UC-3), and **the strip shows the frames**. That
  last part was the gap: reading pixels back *out* of the store had no path across the worker, and
  now it does. `CandidatePreview` answers with a `FramePreview` — a reduced RGBA copy the page puts
  straight into an `ImageData`, at a long edge the client names and the contract bounds. Reduced
  because the arithmetic decides it: a cell's frames are 39 MB against a 128 MB ceiling and its
  previews are 384 KB. Where the reduction happens took a new engine and a new volatility axis
  (V16), because none of the store, the quality engine or the composition engine owns "make this
  frame small enough to look at" (ADR 0038). Reading a preview also puts the frame back in the tier
  it came from, which is the caller ADR 0023 named in advance and left open — a review client
  faulting a sphere's frames in to display them, with no natural finished moment. It has one: the
  reduced copy exists.

  And a recorded selection can be read back, which was the other half. `SetSelection` used to write
  one and no contract returned it, so the override lived in the client's memory and went with the
  tab — the strip came back showing the ranking's pick while the build, which reads the document,
  would use something else. `GetSelection` closes it, and the panel now keeps nothing of its own:
  a zero candidate is the core's answer for "nobody has chosen here", which is a success rather
  than a `NotFound`, because a client that folded the two together would make a project it cannot
  read look like one nobody has edited (ADR 0040). The strip keeps that distinction rather than
  spending it: a cell whose recorded pick could not be read shows its candidates with *none* of
  them claimed to be in force, and says so, because offering the ranking's pick there would be the
  screen disagreeing with the build without a word anywhere.

**Exit:** a full 360×180 capture on a mid-range Android and an iPhone completes without an OOM,
survives a tab reload and resumes, and every cell holds a scored burst. Measured peak memory
recorded per device class.

*Where this stands:* two of the three conditions are met on one device. A Pixel 9 Pro XL captured a
full sphere — 28 of 28 cells, every one holding a scored burst, no OOM — through the deployed
build. The reload-and-resume condition is met in a real browser — an end-to-end test drives it —
but not yet demonstrated on a phone. What is left, and what has landed since:

- **Reload and resume — done, and wired to a button.** The core reads a session document written
  at every committed cell, replans from the spec and lens that document carries, and hands the
  frames it names back to the store through `IFrameStoreAccess::Adopt`, so a restored candidate can
  still be pinned (ADR 0029). Underneath it the OPFS tier survives a reload too: a fixed preferred
  name and a sibling index carrying the frame-to-offset map, with a tier of its own for any session
  that cannot take that pair, which is the property ADR 0020 added (ADR 0030). `Begin` empties the
  tier and `Resume` does not (ADR 0034), so a new capture no longer issues identities the tier is
  already holding frames under and abandoned spheres stop accumulating on disk; and the tier says
  which capture is in it, so a document from another project is refused rather than resumed against
  somebody else's pixels (ADR 0035). And the page now knows: `ProjectSummary` carries `hasSession`,
  filled from the project's session document by the listing the page already makes at load, so a
  capture that was interrupted is offered back rather than discovered by attempting one — a
  successful `Resume` opens the camera, so probing would have started a capture nobody asked for
  (ADR 0036). A resume the core refuses says why and leaves a new capture one press away. An
  end-to-end test captures a cell in a real browser, reloads the tab, presses resume and finds the
  cell and its five candidates still there. Scope, decided with the maintainer rather than assumed:
  in practice there is never more than one unfinished sphere, because resuming means standing in
  the same spot again — so the page offers the newest project that has a session, and only that
  one. The case worth building for is "a call came in mid-capture", not "come back to it tomorrow"
  — so no `navigator.storage.persist()`, and the tier is cleared when a *new* session begins rather
  than at worker startup, which is what lets a reload find its frames still there.
- **A tier generation, so another project's document cannot outlive its pixels — done.** The gap
  ADR 0034 left open was narrow and silent: `Begin` empties the tier and the new capture reissues
  identities from 1, so a session document belonging to a *different* project named frames that
  held someone else's pixels, and the fault-in could not catch it because the bytes were really
  there and really the right length. The tier now carries a token saying which capture is in it —
  minted on every clear, kept in the spill index, recorded in the session document, and compared by
  `Resume`, which refuses a document that names another one and keeps it (ADR 0035). A host with no
  spill tier at all answers zero, which is a token like any other and matches the documents written
  against it, so a desktop and a browser without OPFS still resume what they can.
- **What a refused resume does to the offer — settled.** ADR 0035 expected that when the page's
  resume flow arrived, what it would need was "a project that stops being offered rather than a
  document that has been destroyed". The flow landed alongside it and did neither, and the two ADRs
  were never reconciled on it. They are now, and the answer is close to 0035's instinct but scoped
  differently (ADR 0039): the offer survives every refusal except `Unsupported` — and, since
  ADR 0044, `SensorUnavailable` — and the withdrawal lives in the tab that saw it rather than
  anywhere durable.

  The split is between a refusal about *this attempt* and one about *this build*. A tier this
  device does not currently hold, a store that would not take the frames back, a camera another tab
  is using — those can answer differently on the next press, and a capture still on disk must not
  be made to look gone. `Unsupported` is the core's word for "this build does not read that", and
  no press changes which build is running, so the offer goes rather than inviting a press that
  fails identically every time.

  What makes it a decision rather than a detail is the second half. Writing the refusal down is the
  obvious implementation and it is wrong in exactly the case it exists for: the only thing that
  turns an unreadable document into a readable one is a new version of the app, and a flag in
  `localStorage` or in the project store would survive the update and suppress the offer in the
  first build able to honour it. Held in the DOM it cannot — the tab is gone by then and the offer
  is rebuilt from `hasSession`. The price is that a permanently unreadable document is offered once
  per load, which costs a document read and a sentence and can neither start a capture nor lose
  data. Two consequences came with it: `pump` now hides the resume offer as well as the fresh-start
  button, since the page can reach a state with both live and either would start a render loop; and
  a second press retries the session rather than the enabling, because by then the camera is open
  and the gesture that opened it is spent.
- **A cap on candidates per cell — done.** Found on the iPhone, and the numbers were exact. Motion
  was unavailable there, so guidance never advanced and five bursts landed on one cell: 25
  candidates of 1280×960×4 = 123 MB, against a ceiling of 128 MB — Safari does not report
  `navigator.deviceMemory`, so the store takes the stated fallback (ADR 0023). Ranking is what
  tipped it over, because scoring a cell reads every candidate's pixels and faults the whole
  accumulated set back into the heap at once. Cooling had done its job; the set simply came back.

  A cell now keeps the best eight and forgets the frames of the rest, so a retake competes without
  the ranking growing with it: eight kept plus a burst of five is thirteen frames, 64 MB on that
  phone, half its ceiling (ADR 0037). A frame the caller offered is never forgotten — it is theirs,
  and they still hold the handle — so a cell can still exceed the cap by being offered more frames
  than it, which is the caller's arithmetic to do.

  The number is stated rather than derived from the store's own ceiling, which is the weaker half
  of the decision and is argued in the ADR: what would change it is a device where thirteen frames
  is too many, and the arithmetic to redo it sits beside the constant.
- **A capture needs a motion sensor — decided, and the degraded path is gone.** ADR 0042 made a
  phone with no orientation a supported configuration: guidance targeted by coverage, `ArmBurst`
  declined to enforce a cone it had nothing to measure, and the user aimed by eye. It worked, and
  it took three rounds of review across three layers to make it work.

  What it produced is the reason it is gone. Cells filled in coverage order with whatever the
  camera happened to be pointing at, and nothing verified the two agreed — a folder of pictures
  with a plan's worth of guessed labels, undetectable until a build stage that does not exist yet.
  `RegistrationEngine` is what would make the labels true, and what that needs is not what exists:
  feature extraction landed in Phase 2 — natively only, so not in the browser where this use case
  lives (ADR 0052) — while the matching and frame-to-frame tracking this argument rests on have not.
  Getting there is far future and possibly never. So `Begin` and `Resume` refuse with `SensorUnavailable` before either
  opens a camera, and the user gets a sentence saying what is required and what is missing
  (ADR 0044).

  The second path went with it, which is most of the change: `ArmBurst` enforces the cone
  on two counts rather than one — nothing measured, then outside the cone —
  `StartTracking` always selects `PoseMode::Fused`, `canCapture` and `#capture`
  are gone entirely — the dwell fires every burst and there is no second way — and `beginSession`
  has no "without motion" line to write. Zero `PoseSample.confidence` still exists and now means
  only "no reading yet": a session's first ticks, and a stream carrying rates with no attitude.
  `Locate` keeps its unaimed branch for exactly that, which was worth catching — deleting it, as
  the ADR's first draft said to, would have let a rate-only stream mature a dwell against an
  unmeasured identity and fire a burst at a cell nobody pointed at.

  It cost the browser suite its arrangement, and that was overdue: this runner reports no
  orientation until a test dispatches one, so every capture test in it had been running the
  sensorless path. They now aim at a cell of the plan the core actually made, through the inverse
  of the adapter's own conversion, checked against that conversion rather than assumed.
- **A session that begins and can never capture — open, and recorded rather than closed.** ADR
  0044 refuses a capture where `Capabilities()` says `None`, which is the whole of what a device
  can be asked before a session starts. It does not cover a port that reports a capability and
  then delivers a stream carrying angular rates with no attitude in it: nothing anchors the pose,
  `Locate` never says `HoldStill`, the dwell never matures, and since ADR 0043 the dwell is the
  only thing that arms a burst. The reticle sits parked and the user is told nothing.

  Not reachable from the shipped page — the browser adapter builds every sample from an
  orientation event, so every sample carries an attitude — which is why this is written down
  rather than fixed in the same breath. What would close it is a decision this repo has not
  needed to make: how long a session waits for its first reading before saying so, and whether
  that sentence comes from the manager or the page. `ARateOnlyStreamNeverMaturesADwell` pins the
  half that is settled, which is that such a stream must never be mistaken for an aim.
- **A camera's frame rate is read once and never re-asked — closed by ADR 0045.**
  `maxBurstFps` was taken from `track.getSettings()` inside `open()`, copied into the manager at
  `Begin`/`Resume`, and never invalidated — while `setLocks` drives the `applyConstraints` that
  most often changes it. `ICameraAccess::Capabilities()` is a read-only second call and
  `ArmBurst` uses it, after the locks, on the rule that a capability is re-asked where it is
  consumed. The rule is the deliverable rather than the field: `maxWidth` has the same shape the
  day a track renegotiates.

  The pull is half of it. A resource-access port is resident (ADR 0014) — the host runs in the
  worker and the `MediaStream` is in the page — so `Capabilities()` reads the page's cache rather
  than a track, and the page is what keeps that cache true: it re-reports the camera after
  `setLocks` settles. That re-report is a *refresh* rather than an open — a separate verb on the
  host that updates a camera it has and cannot conjure one it has not — because a push can lose the
  race with the core's own close, and a page guard can only refuse on a close it has been told
  about. Three reviewers found the
  pull-only version of this within minutes of each other, which is why the ADR carries a withdrawn
  rejection rather than a tidy one.

- **The camera port's `EM_JS` seam has nothing pinning its shape — closed by ADR 0045.**
  `host_camera_metric` and `capture-host.ts` agreed by an integer index and a property name, and
  neither was checked: `maxBurstFps` was in the C++ struct for the life of the field, had no
  `case`, and read as zero, which is a legal answer. `ICaptureSessionManager::CameraInUse()` puts
  what the core read back on the boundary, and `every camera capability the core reads crosses the
  seam it reads it through` is the browser test that reads the whole struct back through it — the
  only one in the tree that does. (Not "the only one that runs `BrowserCameraAccess::Open`", which
  this said in two tenses and was never true in either: `Begin` opens the camera, so almost every
  test that clicks `#enable` runs `Open` — almost, because `RequireMotion()` comes first and a
  device with no motion sensor refuses before a camera is asked for, which is ADR 0044's whole
  ordering and what three of those tests assert. What none of them did was look at what `Open`
  answered.) Renaming `case 8` to `case 9` fails it; before, that left the native suite, vitest
  and the browser suite all green with the floor dead.

  Seven of the eight metrics, measured by renumbering each in turn: `supportsTorch` is `false` on a
  runner with no torch whether the metric is read or not, so identity cannot separate the two. What
  covers the rest of the seam is `the camera the core paces a burst by is the one the locks left
  behind`, which pins an exposure — dropping the fake camera to 15 fps — and asserts the core paces
  by 15. Every arm runs `Capabilities()` under wasm — `ArmBurst` calls it unconditionally after a
  successful `SetLocks` — so what is unique about that test is that it is the only one anywhere
  that asserts on the *answer*. The C++ contract suite cannot: it has one implementation and it is
  a fake.

- **A field of view nobody measured is reported as one the camera stated — open, and
  pre-existing.** `CameraCapabilities` now documents `horizontalFovDeg`/`verticalFovDeg` as 0 where
  nothing has been *derived*, rather than where nothing was measured — the sentence was corrected
  on the branch that added ADR 0045, because the old one stated a rule the only real platform
  cannot keep: `deriveFieldOfView` answers a non-zero pair unconditionally, from
  `ASSUMED_LONG_EDGE_FOV_DEG`, including for a 0×0 camera. The correction makes the header honest
  and leaves the gap exactly where it was: there is still no value meaning *nobody measured this*,
  so no reader can tell an assumption from a measurement.

  ADR 0045 did not create this but it did publish it: `CameraInUse()` is on the boundary now and is
  documented as what the camera *reports*, so a status row rendering it shows the user 66° and a
  trigonometric consequence of 66°, attributed to their lens.

  The browser test cannot hold this pair the way it holds the rest — the page adapter's
  `CameraCapabilities` has no field of view at all, since the host derives the pair from resolution
  and a constant, so identity is impossible here rather than merely weak. Its
  `expect(seen.horizontalFovDeg).toBeGreaterThan(0)` cannot fail either, which two reviewers read
  in opposite directions and neither got right. Measured, by renumbering `case 2` off the end and
  rebuilding the core: the metric reads 0, `Begin` refuses with *"the lens field of view is unknown;
  nothing can be tessellated"*, and the test dies fifty lines earlier — as does every browser test
  that opens a camera. So those two metrics are the most strongly pinned in the switch, by the core
  refusing to plan rather than by any assertion.

  What none of that touches is a *wrong constant*, which is the actual defect here and is
  unfalsifiable by construction: nothing in the tree measures the angle, so nothing can disagree
  with 66°.

  The honest fix is on the contract rather than in the test — a way for the struct to say
  "assumed", so the client can label it — and that is a contract change with an ADR behind it.
  Phase 2's bundle adjustment estimates focal length from the frames, which is the only way to
  actually know, and would give the field its first real answer.

- **The white-balance lock has no capability field — open, and pre-existing.**
  `ICameraAccess::SetLocks` takes `lockWhiteBalance`, the page reports `supportsWhiteBalanceLock`
  off the track, and the worker host forwards it as camera metric 6 — but `CameraCapabilities` in
  `contracts/cpp/sphanorama/types.h` has fields for exposure, focus and torch and none for white
  balance, so the browser port reads that metric nowhere and the core cannot know whether the lock
  it is asking for is one the camera offers. Exposure and focus are checked against their fields
  before `SetLocks` is trusted (ADR 0022); white balance is asked for and believed.

  Found while wiring `maxBurstFps` through the same seam, and deliberately not fixed there: adding
  a field to a contract struct is a contract change, which wants an ADR and a decision about what
  a camera that offers no manual white balance should make `SetLocks` do — refuse, or take the two
  locks it can and say which. The answer is probably "the same as exposure", but "probably" is not
  what a contract is for.

- **`contract_gen` reads a sentence about the marker as the marker — open.** `tools/contract_gen.py`
  tests `BOUNDARY_MARKER in d` against each comment line above a class, so
  `IMotionSensorAccess`'s own "not marked `@boundary`, because this contract moves bytes through
  the shared heap" was taken as the mark. The interface was mirrored into
  `contracts/ts/contracts.d.ts` as a declaration nothing imports, and the line that triggered it
  was swallowed on the way, leaving the sentences either side of it joined mid-clause.

  The header is worded around it for now, so the wrong file stops shipping. The fix is to require
  the marker to *begin* a comment's text rather than appear anywhere in it, with a generator test
  for the case that was wrong — small, and worth a change of its own, because marker detection
  decides what crosses the boundary at all and a subtle change there is not something to slip
  into a PR about something else.
- **Two capture loops from three buttons — open, and pre-existing.** Tracked as
  [#50](https://github.com/h3ll5ur7er/sphanorama/issues/50). `pump` says it is "the one
  place that can promise there is only ever one", and nothing enforces that: `#new-capture` stays
  live while `enable` runs, so a refused resume followed by a resume press and then a new-capture
  press starts two loops. Found by a reviewer on PR #49, against ADR 0039's refused-resume flow.

  What it looks like is worse than "two loops", and a later round measured it: `pickUp` writes the
  buttons' visibility while `startFresh` writes `#stage`, both from answers that crossed the
  worker — so the visible end state of that sequence is a **running capture underneath the line
  "a session is already in progress; end it first"**. A user reading that reloads, which costs
  them nothing but is the app telling them it is broken while it works.

  Not fixed there because the honest fix is not local. One guard owning "a loop is starting or
  running" would replace three buttons' worth of `hidden`/`disabled` bookkeeping that `enable`,
  `beginSession`, `pickUp` and `pump` all write, and each of those paths wants a test. That is a
  change to the page's state machine and belongs in a branch of its own rather than inside one
  about motion sensors.
- **A second guard, for the other thing the gate could not see.** A note explaining one axis of
  the volatility map was inserted between two of its rows. A markdown table ends at the first
  blank line, so eleven rows — every engine, resource access and owner from V6 to V16 — stopped
  being a table and rendered as literal pipe text. Nothing about the source looks wrong; only the
  page is broken, and only from the gap down. Five review rounds read the paragraph's prose and
  none rendered the page.

  `tools/markdown_table_check.py` runs beside the conflict-marker checker and on the same
  argument: documentation is a deliverable (ADR 0007), and a table that stops halfway is worse
  than an out-of-date one because it does not read as damage — it reads as a shorter table. It
  compares column counts rather than merely finding pipes, so a diagram drawn with pipes is not a
  finding, and its own first version missed the very document that prompted it until its tests
  said so. **Done.**
- **A guard for the one thing the gate could not see.** A three-way merge left a `>>>>>>>` line in
  this file and the full gate went green over it: the compilers only read C++ and TypeScript, where
  a marker is a syntax error anyway, so the files actually at risk were the ADRs and these notes.
  Documentation is a deliverable here (ADR 0007), and a document carrying a half-finished merge has
  stopped being one. `tools/conflict_marker_check.py` now runs beside the other checkers, and asks
  git which files are in the repository rather than keeping its own list of what to skip. **Done.**
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

Both of the phase's own contract-shaped gaps are closed: the review strip shows the frames rather
than what the core knows about them (ADR 0038), and a recorded selection can be read back rather
than being remembered by the client that wrote it (ADR 0040). What is left in this phase needs a
phone, not a keyboard.

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


### The arrow that "only moves when pointing down" — a real defect, found the second time

Reported from a phone alongside the three-cells-from-one-spot bug. Written down twice: once with
the wrong explanation, and then corrected by review. Both halves are kept, because the wrong one is
instructive.

**The bearing arithmetic is correct**, and this part survived checking. It combines both axes: with
the phone level, a cell 30° left and 30° up gives a bearing of 319.1°, one 30° right and 30° up
gives 40.9°, and one straight up gives 0°. All three reproduce against the shipped engine.

**The first explanation was that the symptom is a deliberate visibility rule** — the arrow is raised
only when the target cell is not in the picture, a level phone always has a cell on screen, so the
arrow is simply absent and "only moves while pointing down" is it only *existing* while pointing
down. That was filed as "measured, not a defect". It was neither.

**What is actually happening: the arrow never hides, so it freezes.** `#target-arrow` carries an
author rule `display: grid`, and the UA stylesheet's `[hidden] { display: none }` is a lower cascade
origin — so the painter setting `arrow.hidden = true` changed nothing that could be seen. The arrow
is on screen at every attitude, including at page load, where a quarter of the glyph sits in the
corner of the app. And because the painter also stops *updating* it when there is no target to point
at, what stays on screen is its last bearing and its last distance, unchanging.

So the report was exact and the explanation inverted it: the arrow was not moving while pointing
down and absent otherwise — it was moving while pointing down and **frozen** otherwise. Fixed by
`#target-arrow[hidden] { display: none; }`, which outranks the rule above it on specificity, with a
browser test that fails without it at the first assertion.

**Two claims in the first write-up were also measured false**, and are worth recording because they
were plausible:

- *"one or two cells are on screen at all times, so the arrow is simply absent."* The first clause is
  right; the second does not follow. The arrow's condition is about the **target** cell, not about
  any cell. Sweeping elevations and azimuths across a few hundred randomly chosen capture states, a
  level phone raised the arrow **roughly a third of the time** — two independent runs read 34.6%
  and 37.5%, and a third 39.4%, which is what a figure sampled over random coverage states does.
  The number is not the point and a single decimal place would be false precision; what matters is
  that it was *not* near zero, and that it was symmetric in elevation, so nothing about it
  distinguished "down". (A reproducible version of this would have to state the capture states it
  sampled, which is the standard the rest of this section is now held to.)

  **Past tense throughout, and that is not a stylistic choice.** Every figure in this bullet was
  measured under the targeting rule ADR 0041 replaced, where the target could be a cell off screen.
  Under the rule that shipped it is zero — see the paragraph below, which is the live number. A
  reviewer read the two forty lines apart and asked which one was true; both are, of different
  builds, and only this sentence said so.
- *"Tilt down, the whole ring leaves the field of view."* At every elevation from −90 to +90 there
  are between one and five cell centres on screen; the view never empties, which is what a
  sphere-covering tessellation means. On a fresh capture the arrow is raised at 0 of 2664 attitudes.
  The intuition came from the overlay unit tests, whose plans have a single cell.

**One sub-case that is genuinely correct.** When the target sits at the camera's own elevation the
bearing is ±90° and does not rotate as the phone pans, because the direction to turn does not
change. The distance does, and the arrow carries it as a number. The exact statement is about the
camera frame rather than about elevation in general: it holds at the horizon and degrades away from
it. With camera and target both at 15° elevation, the bearing reads 83.9°, 75.5°, 58.0° and 18.7° at
azimuth offsets of −45°, −90°, −135° and −170° — sampled points, not an even pan, and quoted that way
because "across a pan" implied a sweep nobody ran. (A fifth reading at −20° was quoted here and has
been dropped: at that offset the target is *on screen*, so `planOverlay` returns no arrow at all and
87.4° is a bearing nothing ever draws. The exact ±90° case is the horizon, which reads 90.0.)

**What was also broken is what the arrow pointed at** — under the old rule, capturing the cell in
front of you moved the target to a neighbour under a still phone. ADR 0041 fixes that, separately.

**And then ADR 0041 made it unreachable, which is the state it is in now.** Guidance names the cell
the camera is *inside*, captured or not, so `targetNode` no longer moves off screen and
`planOverlay`'s `isTarget && !seen.onScreen && !captured` is not met. Measured over 247 attitudes
covering the whole sphere, at three arrangements — a fresh capture, after capturing the cell in
view, and after capturing a neighbourhood, which ought to be the arrow's best case since the nearest
hole is then far away: **raised at none of them.**

The browser test keeps the half that pins the cascade defect — the arrow is hidden when there is
nothing to point at, and every assertion in it fails without `#target-arrow[hidden] { display:
none; }`. The half that asserted the arrow *can* appear is gone, because it rested on the rule
ADR 0041 deleted and there is no arrangement left that raises it.

**Still open, and in a shape somebody can pick up** — tracked as
[#52](https://github.com/h3ll5ur7er/sphanorama/issues/52), because a question this size does not
belong only in a document nobody is assigned.

1. *What should the arrow point at?* This is now the first question rather than the third, because
   the answer decides whether the feature exists. It points at `targetNode`, which since ADR 0041 is
   "the cell you are in" — and a lost user does not need pointing at the cell they are already
   inside. The thing they need is the nearest *hole*, which `Locate` already computes internally and
   does not report. Pointing the arrow at that would make it mean something again and is a small
   change to `planOverlay`. The alternative is that an off-screen indicator has no job once the
   target is always on screen, and the arrow, its CSS and its remaining test come out together.
2. *Should the arrow appear when the target ring is already on screen?* Moot until (1) is answered,
   and kept because it is the same design call from the other side. Today it does not — with the
   freeze fixed the arrow would come and go as the target moves in and out of view, which some
   people read as flicker. The alternatives are: leave it (the ring is the guidance when it is
   visible); always show it; or hold it for a moment after the target comes into view so it fades
   rather than blinks.
   This is a design call and wants a device session, not an argument.
3. *The bearing at the camera's own elevation does not rotate*, correctly, and the distance beside it
   is the only thing that moves. Whether a glyph that holds still while the phone turns reads as
   "correct" or "broken" is again a question for a device rather than for a test.

**The lesson worth keeping.** A measurement can be right and the conclusion drawn from it wrong: the
three bearings were real, and they were used to close a report about something else entirely. The
part nobody measured was the one the user was describing — whether the element is on the screen —
and it took a reviewer with a browser and a `getComputedStyle` to ask. "Measured and not a defect"
is a claim that needs the measurement to be of the thing reported.

**A note on how these were measured.** The browser-derived figures here were first taken in a
checkout whose staged `sphanorama-core.wasm` had been built from the sibling branch — the trap the
engineering skill warns about, walked into while investigating it, and then walked into a second
time while fixing the test that caught it.

It changes the arrow conclusions not at all: that fix is pure CSS, and the hidden-arrow measurements
are taken on a fresh capture, where both branches' targeting rules name the same cell because nothing
is captured yet. It *could* have changed the "roughly a third" figure, which is sampled over random
coverage states and is therefore the one measurement that depends on which targeting rule is in the
core — an earlier version of this note claimed every arrow measurement was taken on a fresh capture,
which was not true of that one. It has since been reproduced independently at 33.5% against this
branch's own core, which is the same "roughly a third".

Recorded because the next person measuring here should rebuild the core for *this* branch first.
`tools/gate.sh` is the only thing that does it as part of a run.

**Open: the pose has no age, and one refusal now depends on it.** `CaptureSessionManager` refreshes
`pose_state_` only on a tick that carried samples, so a sensor that dies mid-session freezes it —
and the page's pump used to ask for guidance only when there were samples or a burst was running, so
a dead sensor froze the whole loop on its last good value. The pump now has a 250 ms heartbeat, so
the *loop* survives — but the pose it is asking about does not, and that is the part still open:
guidance keeps answering, from an orientation nothing has refreshed. Everything downstream then agrees with each
other and with nothing real: the reticle sits on a cell the camera has left, and `ArmBurst`'s aim
check (ADR 0041) *permits* a burst against it, which is that ADR's own failure reached through a
stale pose instead of a stale target. Nothing detects it. `PoseSample.timestampNs` is in the
sensor's time base and `IClock::MonotonicNs` is in the manager's — *in the implementations*. No
contract says so: neither `PoseSample`, `ImuSample` nor `utilities/clock.h` documents an epoch at
all, so "the two do not subtract" is a property nothing prevents a future port from breaking in
either direction. Writing the time bases into the contract is arguably the first half of this work.

The signal that already knows the difference is `guidance.stability`, and it is better than it
looks: the manager does not compute it — it asks `IPoseEngine::Stability` and copies the answer
when it succeeds — and on the sample-less tick this is about, `OrientationPoseEngine` *refuses*
(`FailedPrecondition` on an empty span, deliberately, because reporting stability for a dropout
would let a burst fire blind). So during a freeze the field is not stale, it is absent. Something
has to notice that nobody is asking, decide how old is too old, and say so on screen — a frozen
reticle with no explanation is what a user gets today. It outlived the dwell trigger it was written
to precede: ADR 0043 landed, so a capture now reaches this state without anybody pressing anything,
and the dwell's credit bound limits what a resumed loop can bank rather than saying anything about
what a frozen one reports. The gap this names — nobody deciding how old is too old, and saying so — is still open.

### The stage line has no notion of what supersedes what

Found by round 16's shell-ordering lens. `sayForAWhile` holds a message for a fixed time, and the
guidance-failure branch writes `#guidance` directly — so a line set just before a failure can sit
on screen for up to ~1.1 s after the loop has recovered and repainted the reticle, the horizon and
the markers. The sentence then disagrees with everything around it.

Cosmetic and self-clearing, which is why it was not fixed on the branch that found it: it is a
stale sentence for one second rather than a wrong state. The fix is not a patch either — it means
giving the status line a notion of priority, so a recovery can retract a message a timer is still
holding, and that is a small design decision about the one surface the user reads when something
has gone wrong.

Worth doing before the surface grows: every message added between now and then is another pair
that has to be ordered.

---

## Phase 2 — Stitching
*Goal: a real panorama out the other end.*

- **The lens as maths** — done. `utilities/camera_model` projects a direction to a pixel and back,
  through Brown-Conrady distortion, refusing rather than guessing wherever there is no answer
  (ADR 0046). It is the first code here to read a field of `Intrinsics`, and everything below is
  this transform or its inverse.
- **What "accurate" means** — done, and before anything it measures. `core/test/support/rotation_scoring`
  turns a set of estimated frame rotations into an angular error against known truth, removing the
  global gauge rotation first: a panorama is reconstructed from how frames sit relative to each
  other, so a perfect reconstruction expressed in a world frame 30 degrees from the dataset's would
  otherwise report 30 degrees of error on every frame at once (ADR 0049). The exit criterion below
  names a median, and this is what computes it.
- **Frames to score, and the truth of where they were taken** — done for the geometry.
  `tools/synth_dataset.py` renders the frames a phone would have captured from a panorama and emits
  the rotation each was taken at. It re-implements the lens rather than calling the core, because a
  dataset rendered through the code under test cancels any error the two share and would certify a
  broken projection as accurate (ADR 0050). Noise, blur, rolling shutter, exposure, bursts per cell
  and movers are each still to come.
- `RegistrationEngine`: feature extraction, ratio-test + geometric matching, sensor-prior-seeded
  pure-rotation estimation with RANSAC, then a global bundle adjustment over rotations and shared
  intrinsics (focal + radial distortion).

  *Feature extraction is in* — `FeatureRegistrationEngine::ExtractFeatures` over ORB, AKAZE or SIFT,
  writing descriptors and keypoints into frames the caller owns (ADR 0051). Matching and refinement
  still refuse rather than returning an identity that would look like a registration. It compiles
  only where OpenCV does, so a browser build still has the null engine (ADR 0052), and all three
  detectors share one feature cap — without it two of them are unbounded, which would make the
  comparison below meaningless as well as the memory unbounded.

  **Which detector is a measurement, not a preference.** V7 names ORB, AKAZE and SIFT together on
  purpose. SIFT's patent expired in March 2020 and it has shipped in `features2d` since OpenCV 4.4,
  so it costs no new dependency and the reason it was once excluded no longer exists; what remains
  is a speed-versus-repeatability question on *this* content, at *this* frame size, in
  single-threaded WASM — and that is a number, not an opinion. The accuracy harness below takes the
  detector as a parameter from the start, and the ADR that picks one gets written from what it
  measures, with the speed/quality tiers V7 already anticipates.
- `CompositionEngine`: gain/vignette exposure compensation, graph-cut seam finding, multi-band
  blending, equirectangular projection with tiled output.
- `PanoramaBuildManager`: staged progress, low-res preview first, then full render.
- `ProjectManager` export: JPEG/AVIF with XMP `GPano`.

Listed in dependency order, which is not build order: **the accuracy harness on synthetic datasets
comes first**, before any of the above, for the reason in "What to build first" below. It is last in
this list only because it is the thing that measures the others.

**Exit:** measured on a **native** build, and this needs saying now rather than being assumed.
Registration compiles only where OpenCV does (ADR 0052) and the WASM cross-compile is still deferred
(ADR 0047), so the number below comes from the native build. Whether it transfers to a WASM build of
the same code is a second measurement nobody has taken, and it is not implied by the first: the
single-threaded WASM speed question is explicitly part of what "which detector wins" means here.

Synthetic-dataset registration median error under a stated angular threshold — median
rather than mean because the alignment is fitted to every frame at once, so one outlier smears a
fraction of its error across all the others. The median is far steadier than the mean and is **not**
immune: measured, one frame turned 120 degrees still moves it 0.83 degrees at 61 frames, which is
the same order as any plausible threshold, so whoever states that number needs to read ADR 0049
first; a real capture
exports a file that Google Photos and a WebXR viewer open as a sphere; end-to-end build time recorded
per device class.

The threshold itself is deliberately still blank. It gets stated when the first dataset exists and a
detector has been run against it, because a number chosen before anything can produce one is a number
the implementation will be tuned to rather than measured against.

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
- Degraded modes: no-SAB capture, low-memory device profile. **Not** no-sensor capture — that is
  refused rather than degraded (ADR 0044), and what would reopen it is a registration engine
  good enough to place a live stream of frames without any external reference, which is a
  Phase 3 question at the earliest.

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

**Phase 0's boundary was this section's answer and it is built** — the IDL, the generated facade,
and a core a phone drives with real sensor data. It was the right first commit for the reason
stated at the time: the platform surprises live there (cross-origin isolation, iOS permissions,
heap ceilings) and discovering them in Phase 3 would have been worthless.

The same question for Phase 2 has the same shape, and the answer is **the harness before the
algorithm**. Registration accuracy is invisible to the eye — a rotation that is a degree out
produces a panorama that looks fine until the seam, and by then the cause is three stages back. So
what comes first is a synthetic dataset with known per-frame ground truth and a scored error bound,
because it is the only thing that can tell a regression from a re-tuning, and because the detector
question above is settled by running it rather than by arguing.

The lens (ADR 0046) came before even that, for the same reason again: the harness cannot render a
frame it does not have a projection for.

Still open, and deliberately not yet started: a **real** capture corpus from a phone. Synthetic
data proves correctness because it carries truth; real data finds the assumptions, and has no
ground truth to measure against. They answer different questions and the second is worth nothing
until the first exists.
