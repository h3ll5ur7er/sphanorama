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
| V1 | **How a capture session is sequenced** — reticle order, when a cell is accepted, what the guidance says, retake flow, auto vs manual shutter | Product iteration, UX research, accessibility modes | `CaptureSessionManager` |
| V2 | **How a panorama is built** — pipeline order, which stages run, quality/speed tiers, incremental vs full rebuild | Device class, user's "fast preview vs final render" choice | `PanoramaBuildManager` |
| V3 | **What a saved project is and how its lifecycle runs** — resume, versioning, export targets | Product features, platform sharing APIs | `ProjectManager` |
| V4 | **How the sphere is tessellated and coverage is judged** — ring/FoV layout vs geodesic, overlap targets, hole detection, which cell needs a retake | Lens FoV, capture strategy research, quality bar | `CoveragePlannerEngine` |
| V5 | **How orientation is estimated** — complementary vs Madgwick vs EKF fusion, gyro-only, vision-only, sensor-absent | Device sensor quality, browser API availability, iOS permission denial | `PoseEngine` |
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

## 2.2 Axes deliberately *not* given their own component

| Candidate | Why not |
| --------- | ------- |
| "HDR / bracketing" | It is a variation of V6 (what a burst contains and how candidates combine), plus V8 (how they merge). It gets a strategy inside those, not a service |
| "Stitching quality preset" | A *parameter* of V2, expressed in `BuildSpec` — not a new component |
| "Portrait vs landscape sphere" | A parameter of V4 |
| "Different UI skin / desktop layout" | Client-layer variation. Clients are cheap and expected to multiply |
| "Undo/redo" | A cross-cutting concern over the project document, handled in the utilities bar as an event-sourced journal — see 04 |

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
