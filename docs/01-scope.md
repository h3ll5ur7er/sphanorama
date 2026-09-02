# 1. Scope

## 1.1 Product goals

| # | Goal | Why it shapes the architecture |
| - | ---- | ------------------------------ |
| G1 | Guided sphere capture in a mobile browser, no install | Client layer is a PWA; no native SDK dependency may leak into business logic |
| G2 | **Burst per reticle**, best frame chosen automatically, overridable by the user | A capture cell owns a *set* of candidates, not a frame. Selection is a distinct, swappable algorithm |
| G3 | **Retake any region** without re-shooting the sphere | The build must be incremental and node-addressable, not a monolithic batch stitch |
| G4 | Parallax / moving-object ghosts are detectable and fixable | Multiple candidates per cell are a *signal*, not just a quality pool: disagreement between them localises movers |
| G5 | Everything on-device | No network service in the service map. Storage, compute and export are all local resources |
| G6 | Fast enough to feel like a camera | Heavy work is C++/WASM with SIMD+threads and an optional WebGPU path; the UI thread only marshals |
| G7 | Standard output | Equirectangular JPEG/AVIF with XMP `GPano` metadata, so Google Photos, Facebook and VR viewers read it directly |

## 1.2 Explicit non-goals (v1)

- No server, no account, no cloud stitch fallback.
- No video / 360 video.
- No multi-camera or external camera rigs.
- No HDR bracketing per cell (the burst is same-exposure; HDR is a later, separable concern —
  see the exposure volatility axis, it is designed *for* but not delivered).
- No editing suite (crop, tone curves). Export is the boundary.

## 1.3 Target devices and the constraints they impose

| Constraint | Consequence |
| ---------- | ----------- |
| iOS Safari requires a user gesture for `DeviceOrientationEvent.requestPermission()` | Sensor access is a permission-gated ResourceAccess with an explicit "unavailable" mode; capture must degrade to vision-only pose |
| `SharedArrayBuffer` (needed for WASM threads) requires cross-origin isolation (COOP/COEP), and GitHub Pages cannot send response headers | **The default deployment has no threads.** Two builds ship; Pages gets the single-threaded one. A threaded build served without isolation does not degrade — it hangs on load with no error (ADR [0011](adr/0011-single-threaded-build-for-github-pages.md)) |
| Mobile WASM heaps are bounded (often well under 1 GB) and OOM is fatal | Frames are never all resident. A tiered frame store with spill to OPFS is a first-class service, not an optimisation |
| A 12 MP burst of 8 frames × 40 cells is ~15 GB uncompressed | Bursts stay encoded (or downscaled) until a candidate is selected. Residency policy is explicit |
| WebGPU availability varies by browser/version | Compute backend is an abstracted resource with a CPU/SIMD fallback of equal correctness |
| Phones drift: gyro bias, rolling shutter, unknown lens intrinsics | Camera intrinsics are *estimated*, not configured. Sensor pose is a prior, never ground truth |

## 1.4 Definitions

- **Cell / node** — one target orientation on the sphere that the user is guided to. The unit of
  retake and of incremental rebuild.
- **Candidate** — one frame from a cell's burst, with its pose and quality score.
- **Selection** — the chosen candidate per cell that feeds the build.
- **Build** — the deterministic function from `(plan, selections, poses)` to a panorama.
