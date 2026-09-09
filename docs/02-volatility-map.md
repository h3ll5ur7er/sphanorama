# 2. Volatility analysis

iDesign decomposes by **what is likely to change**, not by what the system does. Functional
decomposition ("CaptureService, StitchService, ExportService") would spread every future change
across every component. Below is the axis list that produced the service map in
[03-architecture.md](03-architecture.md).

Each axis names: what varies, over what dimension it varies, and which single component is meant
to absorb that variation. If a change requires touching two components, one of them is in the
wrong place.

## 2.1 Axes of volatility

| # | Volatility | Varies over | Encapsulated by |
| - | ---------- | ----------- | --------------- |
| V1 | **How a capture session is sequenced** — reticle order, when a cell is accepted, what the guidance says, retake flow, auto vs manual shutter, **and which devices a capture is served on at all** | Product iteration, UX research, accessibility modes, what the pipeline below can honestly do with a frame | `CaptureSessionManager` |
| V2 | **How a panorama is built** — pipeline order, which stages run, quality/speed tiers, incremental vs full rebuild | Device class, user's "fast preview vs final render" choice | `PanoramaBuildManager` |
| V3 | **What a saved project is and how its lifecycle runs** — resume, versioning, export targets | Product features, platform sharing APIs | `ProjectManager` |
| V4 | **How the sphere is tessellated and coverage is judged** — ring/FoV layout vs geodesic, overlap targets, hole detection, which cell needs a retake | Lens FoV, capture strategy research, quality bar | `CoveragePlannerEngine` |
| V5 | **How orientation is estimated** — complementary vs Madgwick vs EKF fusion, gyro-only, vision-only | Device sensor quality, browser API availability | `PoseEngine` |
| V6 | **What makes a frame "the best of the burst"** — sharpness metric, motion-blur estimate, exposure agreement, mover-content penalty, user override | Algorithm research; this is the feature most likely to be tuned | `FrameQualityEngine` |
| V7 | **How frames are aligned** — ORB/AKAZE/SIFT, matcher, RANSAC model (homography vs pure rotation), sensor-prior weighting, global refinement | Algorithm research, speed/quality tiers | `RegistrationEngine` |
| V8 | **How pixels become one image** — exposure compensation, seam finding, ghost masking, blend (feather/multiband), projection (equirect/cubemap), resampling | Algorithm research and output format | `CompositionEngine` |
| V9 | **Where camera frames come from** — `getUserMedia` + `ImageCapture` vs WebCodecs `VideoFrame` vs a file-import test source | Browser API churn, headless testing | `ICameraAccess` |
| V10 | **Where motion data comes from** — `DeviceOrientation`, `DeviceMotion`, Generic Sensor API, replayed log, none | Browser/OS, permissions | `IMotionSensorAccess` |
| V11 | **Where pixel bytes live** — WASM heap, GPU texture, OPFS spill file, encoded blob | Memory pressure, device class | `IFrameStoreAccess` |
| V12 | **Where project metadata is persisted** — IndexedDB, OPFS, in-memory (tests) | Platform quota behaviour | `IProjectStoreAccess` |
| V13 | **How images are decoded/encoded and metadata written** — browser codecs vs libjpeg-turbo in WASM, JPEG vs AVIF, XMP GPano | Format support, output requirements | `IImageCodecAccess` |
| V14 | **Where heavy math executes** — scalar C++, WASM SIMD, WASM threads, WebGPU compute | Device capability | `IComputeDeviceAccess` |
| V15 | **How a result leaves the device** — download, Web Share, File System Access, clipboard | Platform APIs | `IExportAccess` |
| V16 | **How a stored frame is made small enough to look at** — the reduction factor, the filter, the pixel format it lands in | The surface doing the reviewing, and what a crossing costs | `FramePreviewEngine` |

Sensor *absence* moved out of V5 and into V1 with ADR 0044. It was V5's while the engine could absorb it — vision-only mode, and nothing above needed to know. It is not an estimation strategy any more but a decision about whether to open a session at all, which is V1's, and `CaptureSessionManager::RequireMotion` is where it lives. `PoseMode::VisionOnly` stays in the contract for the day `RegistrationEngine` can carry one, at which point it comes back to V5.

**How registration accuracy is judged** is an axis this map does not carry a row for, and that is
deliberate rather than an omission. It varies — the metric could be chordal or geodesic, the summary
could be median or mean or a percentile, the gauge could be quotiented or pinned — but every one of
those choices is a fact about how we *test*, not about what the app does on a phone. Nothing in
`core/src` reads it, and a row here would imply a shipped component that has to exist. ADR 0049
holds the decision; the day a bench client or a shipped feature needs the same number, it earns a
row and a `utilities/` home together.

**How a synthetic dataset is produced** — the panorama it is rendered from, the lens model, the pose
trajectory, the noise and blur and rolling-shutter and exposure models still to come — varies as much
as anything in this map and has no component here either, for the same reason as the axis above it:
it is a fact about how we test, `tools/synth_dataset.py` owns it, and nothing in `core/src` reads it.
ADR 0050 holds the decision, including why that tool implements the lens itself rather than calling
`camera_model`.

## 2.2 Axes deliberately *not* given their own component

| Candidate | Why not |
| --------- | ------- |
| "HDR / bracketing" | It is a variation of V6 (what a burst contains and how candidates combine), plus V8 (how they merge). It gets a strategy inside those, not a service |
| "Stitching quality preset" | A *parameter* of V2, expressed in `BuildSpec` — not a new component |
| "Portrait vs landscape sphere" | A parameter of V4 |
| "Different UI skin / desktop layout" | Client-layer variation. Clients are cheap and expected to multiply |
| "Undo/redo" | A cross-cutting concern over the project document, handled in the utilities bar as an event-sourced journal — see 04 |
| "A thumbnail is just a small composition" | It was the closest existing owner and it is not one. V8 is *many frames becoming one image* and every method of `ICompositionEngine` takes a `GlobalSolution`; a per-frame reduction for a screen shares the word "resampling" and nothing else, and putting it there would have a capture session calling the composition engine (ADR 0038) |

## 2.3 The trap this avoids

The obvious functional split is `Capture → Stitch → Export`. Under it, the three headline
features of this product land badly:

- *Burst per reticle* changes Capture **and** Stitch (which frame is "the" frame).
- *Retake a region* changes Capture **and** Stitch (partial invalidation) **and** Export.
- *Ghost removal* changes Stitch **and** requires Capture to have kept more than one frame.

Under the volatility decomposition, each lands in one place: `FrameQualityEngine` (V6),
`PanoramaBuildManager` incremental invalidation (V2), and `CompositionEngine` ghost masking (V8),
with `CaptureSessionManager` merely sequencing. That separation is the entire point of the
exercise.
