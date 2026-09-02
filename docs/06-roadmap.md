# 6. Delivery plan

Each phase has a demo and an exit criterion. Nothing is "done" because code exists; it is done when
the criterion is measurable. Phases are ordered so that the riskiest unknowns (memory ceilings,
sensor quality, iOS behaviour) are hit early, not at the end.

---

## Phase 0 — Skeleton and toolchain
*Goal: a build that proves the boundary works, containing no algorithms at all.*

- Repo layout, CMake presets, pinned emsdk, trimmed OpenCV WASM build, Vite PWA shell.
- Utilities bar: `Result<T>`, status codes, logger, clock, config, arena.
- The generated boundary: IDL → C++ facade + TS proxy + FlatBuffers types.
- Null implementations of all three managers and all five engines; real `ICameraAccess` and
  `IMotionSensorAccess` adapters.
- The test machinery the rest of the plan depends on: GoogleTest harness, the fake
  `ICameraAccess`/`IMotionSensorAccess` pair backed by a frame folder and a recorded IMU log,
  and the synthetic-dataset generator in `tools/`.
- CI: layer check, contract-drift check, no-browser native build, size budget.

**Exit:** a phone opens the PWA, sees a live viewfinder with a reticle whose position is driven by
real sensor data routed *through the WASM core*, and the whole round trip stays under budget.
The core binary is under 8 MB and the same core compiles as the native bench.

*Where this stands:* the walking skeleton is up — the PWA loads the WASM core, reports its
capabilities, opens the camera and streams live orientation, and deploys to GitHub Pages. The
three managers and five null engines are in, with the capture sequence from
[UC-1](03-architecture.md) driven end to end against fakes.

What is missing before the exit criterion is met: the generated manager facade, so the client
reaches the capture session rather than only the capability probe; and a real
`CoveragePlannerEngine`, so the reticle follows a coverage plan instead of a sensor readout. The
null planner deliberately plans one cell at identity — enough for the sequence to have a target,
and obviously not a tessellation.

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
