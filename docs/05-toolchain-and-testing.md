# 5. Toolchain, languages and validation

The *why* behind the practices here — tests first, docs as deliverables, enforced layer
rules — is in [`00-principles.md`](00-principles.md). This document covers the machinery that
makes them checkable.

## 5.1 Language allocation

| Language | Used for | Rationale |
| -------- | -------- | --------- |
| **C++20** | Managers, engines, resource-access contracts, native resource-access implementations | The whole point: one implementation of the business logic, compiled to WASM for the browser and to a native binary for the bench. OpenCV is C++ |
| **TypeScript** | Clients, browser resource-access adapters, PWA shell, service worker | Thin by design. If a `.ts` file contains geometry or pixel maths, it is in the wrong layer |
| **Python**, run through `uv` | Contract codegen, synthetic dataset generation, reference implementations | The auxiliary language. Nothing shipped to the device is written in it. `uv run tools/…` everywhere, with `pyproject.toml` and a committed `uv.lock` (ADR 0048) — dependencies are added with `uv add`, never pip. The dataset renderer is the exception to the invocation: it needs `uv run --group datasets tools/…`, because numpy is in a group so the checkers stay standard-library only (ADR 0050). Result scoring is *not* here — ADR 0049 put it in C++, in `core/test/support/rotation_scoring` |

No Rust/Swift/C# — nothing in the design needs them, and each would add a toolchain without
removing one.

## 5.2 Build

- **Emscripten** (pinned via `emsdk`) with `-msimd128`, `-pthread`, `-sALLOW_MEMORY_GROWTH`,
  `-sEXPORT_ES6`. Two builds from one source tree — `wasm-release` and `wasm-release-threaded` —
  each producing `sphanorama-core.wasm` and its glue `sphanorama-core.js` in its own build
  directory. **Which one ships is a deploy decision, not a runtime one**: GitHub Pages serves no
  COOP/COEP headers, so it gets the single-threaded build (ADR 0011), and `npm run build` stages
  that one. This paragraph described `core.wasm` / `core.st.wasm` "selected at runtime by capability
  probe" until seven rounds of review on PR #67 swept the file — neither name nor probe has ever
  existed. `IComputeDeviceAccess::Capabilities` does report `threads`, which is probably where the
  idea came from, but nothing chooses an artefact from it.
- **CMake** presets, all six: `native-debug` (the default for TDD), `native-asan` (the sanitizer
  job), `wasm-release` and `wasm-release-threaded` (both built by the gate and by CI), and two that
  nothing builds automatically — `wasm-debug`, for a person chasing something in the browser, and
  `native-release`, for a timing run somebody does by hand. This line used to gloss the latter as
  "what the bench measures", which was false twice over: `bench/` does not exist (ADR 0052 says so
  while explaining why no composition root selects the OpenCV engine), and neither `tools/gate.sh`
  nor CI configures that preset at all.
- **OpenCV** built from source as a trimmed static subset — `core`, `imgproc`, `features2d`,
  `calib3d`, `photo`, `flann` — fetched at a pinned *commit* by `cmake/opencv.cmake`, and verified
  against that commit after checkout rather than trusted (ADR 0047). The
  `stitching` module is deliberately *not* used wholesale: it is a monolith that would swallow V7 and
  V8 into one opaque dependency and make incremental rebuild impossible. We use its algorithms
  piecemeal behind our own engine contracts.

  **Native today, WASM later.** `SPHANORAMA_WITH_OPENCV` is on for native builds and forced off under
  Emscripten: cross-compiling the subset has its own size budget and its own failure modes, and
  nothing about writing the algorithms needs it in a browser first.
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
   UndefinedBehaviorSanitizer. Both jobs build OpenCV from source, so both cache `_deps` *and*
   `.ninja_log` — ninja marks any output with no log entry dirty, so the cache did nothing without
   the log. That is also why the key carries a toolchain identity (compiler, ninja and cmake
   versions) and not just the runner OS: restoring the log restores ninja's belief that those
   objects are current, and a runner image rotating to a different compiler would otherwise link
   objects nothing can notice are stale.

   The sanitizer preset sets `CMAKE_CXX_FLAGS` globally, so OpenCV's translation units are
   instrumented too, under `-fno-sanitize-recover=all` — with exactly one check lifted from them.
   That day arrived: the first engine to call into `features2d` did turn it red, in `cv::resize`
   inside ORB's pyramid, where OpenCV gathers source pixels through `*(const short*)(row + index)`
   at an index that is odd for half of all inputs. `cmake/opencv.cmake` appends
   `-fno-sanitize=alignment` around OpenCV's `add_subdirectory` and restores the flags after, so our
   own translation units keep the check — measured both ways, present on OpenCV's command lines and
   absent from ours.

   ADR 0047 predicted the day and named a different remedy, a suppressions file scoped to
   `_deps/opencv-src`, "not turning recovery back on". ADR 0052 records why that pair is not
   available: a UBSan suppressions file is only consulted for *recoverable* errors, so it does
   nothing under `-fno-sanitize-recover=all` — measured, and the compile-time flag is what reaches
   0047's actual goal without trading the thing it did not want to trade.
4. **No-browser check** — `tools/no_browser_check.py` rejects any reference to Emscripten,
   inline JavaScript or WebAssembly build macros outside `bridge/`. Deliberately blunt: the bare
   word in a comment counts, because "on Emscripten we do X" means the core is reasoning about a
   platform it is supposed to know nothing about.
5. **WASM builds and size budget** — both artifacts (ADR 0011) built and measured gzipped against
   `tools/size_budgets.toml`. A missing artifact fails rather than passes, so the budget cannot
   look green when the build produced nothing.
6. **Browser tests** — the shipped modules loaded in headless Chromium, served with *and* without
   COOP/COEP, which is the only way to find out what the deployment target does with them.
   `tools/check_dist_fresh.mjs` is their precondition rather than a step of its own: Playwright's
   `globalSetup`, refusing to run the suite against a bundle or a core older than the sources it
   was built from. It compares both wasm presets and the glue `.js` against the C++ that the wasm
   build actually compiles — read from each preset's `compile_commands.json`, because since ADR 0052
   a core source can be native-only, and a source ninja never builds could otherwise make the core
   permanently stale — and the build
   files, and it exists because two false-green sabotage runs got through — one after
   `npm run build` had exited non-zero on a typecheck error and left the previous `dist` standing.
7. **Conflict markers** — `tools/conflict_marker_check.py`, because a merge marker in a tracked
   file is a file nobody finished reading.
8. **Broken tables** — `tools/markdown_table_check.py`. A paragraph between two rows closes a
   GitHub-flavoured table, and eleven rows of the volatility map rendered as pipe text for five
   review rounds because everybody read the prose and nobody rendered the page.
9. **Dataset renderer tests** — `tools/test_synth_dataset.py`, in the `contracts` job. It predates
   this list's last revision and was simply missed; it is here because the renderer is checked
   against hand-worked decimals rather than against the code it feeds (ADR 0050), so a change to it
   is a change to what every accuracy figure means.
10. **The accuracy measurement actually ran** — in *both* the `native` and `sanitizers` jobs. Not a
    checker but a guard on one: the measurement skips without `uv`, and `ctest` reported "100%
    tests passed" while running none of it. The step derives the expected count from
    `--gtest_list_tests` and fails on a skip, a shortfall or a floor of zero.

**This list has been short before, and that is the argument for the sentence below it.** Items 9 and
10 were missing until the branch was reviewed as a whole against `main` rather than by commit range
— 9 predating the branch entirely. `docs/00-principles.md` and `README.md` both promise a reader
that `tools/gate.sh` mirrors `.github/workflows/ci.yml` step for step, so a list here that is not
the list makes that promise false one level up.

Every checker in that list has its own test suite, `check_dist_fresh` included, and `gate.sh` runs
most of them immediately before the check they guard. The reason is in that file's own header:
the checkers never change while you are working, which is exactly what makes a broken one the
easiest thing not to notice. Two cannot be adjacent, and it is worth saying which
rather than claiming a tidiness the file does not have: `test_size_budget.py` runs with the other
checker suites at the top, seventeen steps before the budget it guards — fifteen until this branch
inserted `accuracy measured` and `asan accuracy` between them — because that budget needs a
wasm build — and in CI the two are different jobs; `check_dist_fresh.test.mjs` runs inside
`npm test`, with `npm run build` between it and the Playwright run it gates.

Every job that runs a checker sets up `uv` rather than a bare interpreter (ADR 0048), and invokes
the tools as `uv run --locked`: `--locked` fails rather than silently re-resolving, so a `uv.lock`
that no longer matches `pyproject.toml` is a red build rather than a CI run on dependencies nobody
recorded.

Not yet wired, and deliberately absent from CI rather than stubbed green:

- **FlatBuffers schema** — generated from the same parse as the contract mirror, for zero-copy
  reads across the worker boundary. Needs the boundary runtime.
- **Time-to-first-viewfinder budget** — needs the PWA shell.

## 5.4 Test strategy

Tests are written before the code they cover ([`00-principles.md` §0.2](00-principles.md)),
so this table is as much a description of how work starts as of what CI runs. The two rows
that carry the most weight are *engine accuracy* — because correctness here is invisible to the
eye — and *manager behaviour*, which is only cheap because resource access is a contract rather
than a browser call.

| Level | What | How |
| ----- | ---- | --- |
| Engine unit | Pure functions with fixed inputs | GoogleTest, native, with inputs written in the test. One dataset is committed — `core/test/data/synthetic-ring-4`, a *format* fixture for the loader (ADR 0053) — and it is the only one; an earlier version of this row said "golden outputs checked in as small fixtures", which described a practice this repository has never followed: no golden outputs have ever been checked in. It was already false before this branch; what this branch added was a line in `docs/00-principles.md`'s repo map — "the one committed dataset" — that makes the two contradict each other on the page, which is how it was finally noticed |
| Engine accuracy | "Is the estimated rotation right?" | `core/test/support/rotation_scoring` scores a set of estimated rotations against known truth with the global gauge removed first, and reports a median (ADR 0049) — **built**. The synthetic datasets of §5.5 that feed it are built (geometry and ground truth; §5.5 lists what is not), and `core/test/support/synthetic_dataset` now reads one into a frame store, so the two are joined rather than merely both present (ADR 0053). `FeatureRegistrationEngine::EstimatePairwise` is the thing that estimates rotations for it to score, and `core/test/engines/registration_accuracy_test.cpp` joins all four: it renders a ring, registers each consecutive pair against a perturbed prior, chains the answers and asserts a median, a maximum and the share of pairs that registered at all. **Two different numbers live here and they are not interchangeable**: `docs/06-roadmap.md` states 0.5 degrees as what *Phase 2 exits on*, a claim about what a panorama needs, while this test asserts `medianDeg < 0.2` and `maxDeg < 0.4` — regression bounds set at roughly twice the measurement, whose job is to notice the estimator getting worse. A bound generous enough to be a product statement is far too loose for that, and the test says so at its own assertions; this row used to quote only the 0.5 and so repeated the conflation a reviewer had already made the test stop making. Still to come: `Refine`, so the score is over a global solution rather than a chain |
| Manager behaviour | Sequencing and state machines | Native tests with **fake** resource accesses (a recorded IMU log + a folder of frames implements `IMotionSensorAccess`/`ICameraAccess` exactly). This is why those are contracts and not `getUserMedia` calls |
| Boundary | Facade marshalling, error codes | Vitest against the real WASM module in Node |
| Client | Reticle logic, guidance rendering | Vitest + Testing Library, with a mocked manager proxy |
| End-to-end | Full capture → build → export | Playwright driving headless Chromium with a fake media device (`--use-file-for-fake-video-capture`) and replayed sensor logs |
| Perf | Per-stage timings on device classes | Bench client for native numbers; a Playwright trace budget for the browser |

## 5.5 Synthetic datasets — the thing that makes any of this verifiable

`tools/synth_dataset.py` renders the frames a phone *would* have captured from an equirectangular
panorama: given a lens field of view and a list of camera orientations, it emits one image per
orientation **plus the ground-truth rotation of every frame**, as binary Netpbm beside a
`truth.json`. It runs through the `datasets` dependency group, which is what carries numpy — the
checkers above stay standard-library only (ADR 0050).

**It implements the lens itself rather than calling the core**, so a dataset is never rendered by
the code it will be used to measure — an error the two shared would cancel, and the harness would
score a broken projection as perfect. What carries the weight is not the separation, though: a
reviewer showed the arithmetic is close enough to the core's to be called a transcription. It is the
pinning of both to hand-worked decimals derived from neither (ADR 0050).

Built so far: the geometry, the equirectangular sampling with a wrapping seam, ground truth, and a
procedural panorama. Still to come, each its own increment with its own invariant: real HDRIs, a
noise and blur model, rolling-shutter skew, an exposure ramp, a burst per cell, and composited
movers for known ghost regions.

It gives none of these *yet*, and the first is the only one whose machinery is complete. A reviewer
pointed out that "what it gives today is the first of these" — which is what this line used to say —
is contradicted by the bullet immediately under it:

- registration accuracy measured in degrees against truth, not eyeballed — all three parts that
  make it possible are in now (`rotation_scoring`, ADR 0049; `tools/synth_dataset.py`, ADR 0050;
  and `core/test/support/synthetic_dataset`, ADR 0053, which reads what the second writes into a
  frame store the first can be run over). What is missing is no longer plumbing:
  `FeatureRegistrationEngine` extracts features **and estimates pairwise rotations**, and those
  rotations are scored: see the measured table in `docs/06-roadmap.md`. `Refine` still refuses, so
  what is scored is a chain of pairwise estimates rather than a global solution, and the datasets are
  still geometry-only. Twice now this bullet has been wrong in opposite directions — "available now"
  overcorrected a stale sentence, and the correction outlived the code that made it true, surviving a
  whole branch that added the measurement because the branch never opened this file;
- ghost detection scored against a known mask (needs the movers);
- a reproducible regression suite that costs nothing to re-shoot;
- fixtures for the fake `ICameraAccess`, so managers can be tested end-to-end without a camera.

Real captures from real phones are collected alongside it as a smaller, harder corpus — synthetic
data proves correctness, real data finds the assumptions.

## 5.6 Repository layout

See [`00-principles.md` §0.6](00-principles.md). Placement follows layer, not feature.
