# 5. Toolchain, languages and validation

The *why* behind the practices here — tests first, docs as deliverables, enforced layer
rules — is in [`00-principles.md`](00-principles.md). This document covers the machinery that
makes them checkable.

## 5.1 Language allocation

| Language | Used for | Rationale |
| -------- | -------- | --------- |
| **C++20** | Managers, engines, resource-access contracts, native resource-access implementations | The whole point: one implementation of the business logic, compiled to WASM for the browser and to a native binary for the bench. OpenCV is C++ |
| **TypeScript** | Clients, browser resource-access adapters, PWA shell, service worker | Thin by design. If a `.ts` file contains geometry or pixel maths, it is in the wrong layer |
| **Python** | Contract codegen, synthetic dataset generation, reference implementations, result scoring | The auxiliary language. Nothing shipped to the device is written in it |

No Rust/Swift/C# — nothing in the design needs them, and each would add a toolchain without
removing one.

## 5.2 Build

- **Emscripten** (pinned via `emsdk`) with `-msimd128`, `-pthread`, `-sALLOW_MEMORY_GROWTH`,
  `-sEXPORT_ES6`. Two artefacts from one source tree: `core.wasm` (threaded, cross-origin isolated)
  and `core.st.wasm` (single-threaded fallback), selected at runtime by capability probe.
- **CMake** presets: `wasm-release`, `wasm-debug`, `native-debug` (bench + tests), `native-asan`.
- **OpenCV** built from source for WASM as a trimmed static subset — `core`, `imgproc`,
  `features2d`, `calib3d`, `photo`, `flann`. The `stitching` module is deliberately *not* used
  wholesale: it is a monolith that would swallow V7 and V8 into one opaque dependency and make
  incremental rebuild impossible. We use its algorithms piecemeal behind our own engine contracts.
- **Vite** for the PWA, `workbox` for the service worker, plus a COOP/COEP shim service worker for
  hosts that cannot set the headers (GitHub Pages).
- Binary size budget: **< 8 MB** compressed for the core, enforced in CI. It is a phone over
  mobile data.

## 5.3 Architecture enforcement in CI

The call rules in §3.3 are only real if they fail a build:

1. **Layer check** — a Python script builds the include/call graph of the core and rejects any
   edge that violates the matrix (client→engine, manager→manager, engine→manager, engine→any
   resource access other than compute/frame-store).
2. **Contract drift check** — regenerate the TS mirror and FlatBuffers schema; fail on diff.
3. **No-browser check** — the native bench build must compile the entire core with zero Emscripten
   or JS symbols. If it does not, browser assumptions have leaked into business logic.
4. **Size and startup budget** — wasm size, and time-to-first-viewfinder measured in headless
   Chromium.

## 5.4 Test strategy

Tests are written before the code they cover ([`00-principles.md` §0.2](00-principles.md)),
so this table is as much a description of how work starts as of what CI runs. The two rows
that carry the most weight are *engine accuracy* — because correctness here is invisible to the
eye — and *manager behaviour*, which is only cheap because resource access is a contract rather
than a browser call.

| Level | What | How |
| ----- | ---- | --- |
| Engine unit | Pure functions with fixed inputs | GoogleTest, native. Golden outputs checked in as small fixtures |
| Engine accuracy | "Is the estimated rotation right?" | Synthetic datasets (§5.5) with known ground-truth pose; assert angular error below a threshold. Compared against a Python/OpenCV reference so a regression is distinguishable from a re-tuning |
| Manager behaviour | Sequencing and state machines | Native tests with **fake** resource accesses (a recorded IMU log + a folder of frames implements `IMotionSensorAccess`/`ICameraAccess` exactly). This is why those are contracts and not `getUserMedia` calls |
| Boundary | Facade marshalling, error codes | Vitest against the real WASM module in Node |
| Client | Reticle logic, guidance rendering | Vitest + Testing Library, with a mocked manager proxy |
| End-to-end | Full capture → build → export | Playwright driving headless Chromium with a fake media device (`--use-file-for-fake-video-capture`) and replayed sensor logs |
| Perf | Per-stage timings on device classes | Bench client for native numbers; a Playwright trace budget for the browser |

## 5.5 Synthetic datasets — the thing that makes any of this verifiable

A Python tool takes an existing high-resolution equirectangular panorama (public-domain HDRIs) and
renders the exact frames a phone *would* have captured: given a lens FoV, a pose trajectory, a
noise/blur model, rolling-shutter skew and an exposure ramp, it emits a burst per cell **plus the
ground-truth rotation of every frame**. Optional composited movers produce known ghost regions.

That gives:

- registration accuracy measured in degrees against truth, not eyeballed;
- ghost detection scored against a known mask;
- a reproducible regression suite that costs nothing to re-shoot;
- fixtures for the fake `ICameraAccess`, so managers can be tested end-to-end without a camera.

Real captures from real phones are collected alongside it as a smaller, harder corpus — synthetic
data proves correctness, real data finds the assumptions.

## 5.6 Repository layout

See [`00-principles.md` §0.6](00-principles.md). Placement follows layer, not feature.
