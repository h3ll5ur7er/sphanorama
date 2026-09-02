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

The call rules in §3.3 are only real if they fail a build. What runs today:

1. **Layer check** — `tools/layer_check.py` walks the include graph of `core/` and `contracts/`
   and rejects any edge the matrix forbids: client→engine, manager→manager, engine→manager,
   engine→any resource access other than compute and frame store, and reaching sideways into a
   sibling component's private headers. It has its own test suite, run first: a checker that
   passes everything is worse than no checker, because it reads as a green light.
2. **Contract drift** — `tools/contract_gen.py --check` regenerates the TypeScript mirror from
   the C++ headers and fails on any diff. It too has its own suite, including a strictness suite:
   what the generator *refuses* matters more than what it emits.
3. **Native build and tests** — debug, plus a second pass under AddressSanitizer and
   UndefinedBehaviorSanitizer.

Not yet wired, and deliberately absent from CI rather than stubbed green:

4. **No-browser check** — assert the native build of the core contains zero Emscripten or JS
   symbols. Needs a WASM target to contrast against.
5. **FlatBuffers schema** — generated from the same parse as the mirror, for zero-copy reads
   across the worker boundary. Needs the boundary runtime.
6. **Size and startup budget** — WASM size, and time-to-first-viewfinder in headless Chromium.
   Needs emsdk.

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
