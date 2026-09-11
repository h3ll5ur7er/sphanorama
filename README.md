# Sphanorama

An on-device photo sphere capture and stitching PWA. Open it in a phone browser, walk the
reticles around you, and get a full equirectangular panorama — with burst capture per reticle,
per-region retakes to kill parallax ghosts, and **no pixel ever leaving the device**.

Phase 0 is complete: the native and WASM builds, the generated boundary, the three managers, a real
coverage planner and pose engine, and the Pages deploy are in and green. A phone opens the app and
the reticle it sees is a coverage plan the core built and guidance the core returned. Capture is
wired to pixels since Phase 1 — the page transfers preview frames into the core's frame store, and
the core ranks a burst and decides when to fire. What is not wired is stitching: see
[docs/06-roadmap.md](docs/06-roadmap.md) for what is missing and why.

The architecture came first on purpose, following the
[iDesign Method](https://www.idesign.net) (decomposition by volatility, layered service map,
contracts defined before code), and the layer rules are enforced in CI rather than trusted.

## Why

Google Photo Sphere was removed from the Pixel camera. It was good, but it had three limitations
worth fixing:

1. **One shot per reticle.** A blink, a bump, or a passing car poisoned a cell permanently.
2. **No retakes.** A ghosted region meant re-shooting the whole sphere.
3. **Locked to one vendor's phone.** It should be a URL.

## Read in this order

| Doc | What it answers |
| --- | --- |
| [docs/00-principles.md](docs/00-principles.md) | How we build: TDD, docs as deliverables, definition of done, where a new thing goes |
| [docs/01-scope.md](docs/01-scope.md) | What we are building, what we are explicitly not building, target devices |
| [docs/02-volatility-map.md](docs/02-volatility-map.md) | The volatility analysis that drives the decomposition |
| [docs/03-architecture.md](docs/03-architecture.md) | The service map, layers, call rules, use-case walkthroughs |
| [docs/04-runtime-topology.md](docs/04-runtime-topology.md) | Threads, workers, memory tiers, the WASM/JS boundary, data model |
| [docs/05-toolchain-and-testing.md](docs/05-toolchain-and-testing.md) | Languages, build, validation strategy |
| [docs/06-roadmap.md](docs/06-roadmap.md) | Phased delivery plan with exit criteria |
| [docs/adr/](docs/adr/) | Decision records for the choices that are expensive to reverse |
| [contracts/](contracts/) | The interface contracts themselves (C++ headers + mirrored TS types) |

The same principles are packaged as a project skill at [`.claude/skills/sphanorama-engineering/`](.claude/skills/sphanorama-engineering/SKILL.md), so Claude Code sessions working in this repo pick them up automatically.

## Shape at a glance

- **Core** — C++20 compiled to WebAssembly (SIMD + threads) via Emscripten. Holds all Managers,
  Engines and ResourceAccess *contracts*. It uses OpenCV for features and geometry from Phase 2 on,
  but **only where OpenCV is linked, which is not the browser**: the WASM cross-compile has its own
  size budget and is deferred, so a browser build gets a null registration engine (ADR 0047,
  ADR 0052). Blending is not written yet in either build.
- **Shell** — a thin TypeScript PWA. Camera, motion sensors, storage, and the capture UI. Supplies
  concrete ResourceAccess adapters to the core; contains no business logic.
- **Tooling** — Python, run through `uv`, for contract codegen, the architecture checks CI runs, and
  synthetic dataset generation (`tools/synth_dataset.py`, ADR 0050 — the one tool with a dependency,
  named at its call site so the checkers stay standard-library only). Scoring a reconstruction
  against ground truth is C++ rather than Python, because it belongs beside the tests that read it
  (ADR 0049).

## Building it

Four things have to be on `PATH`, and the first two are the ones people do not already have:

| Tool | Why |
| --- | --- |
| [`uv`](https://docs.astral.sh/uv/) | Every Python tool runs through it (ADR 0048). The interpreter and dependencies come from `pyproject.toml` and `uv.lock`, so your machine resolves what CI resolves |
| [Emscripten](https://emscripten.org/) 6.0.9 | The WASM builds. `tools/setup_emsdk.sh` installs the pinned version |
| CMake ≥ 3.25 and Ninja | All six presets — three native, three WASM |
| Node 22 | The shell, its unit tests and the Playwright suite |

Then:

```sh
npm ci                 # shell dependencies, including the browser Playwright drives
tools/setup_emsdk.sh   # once, unless emcc is already on PATH
tools/gate.sh          # everything CI runs, in the order CI runs it
```

`tools/gate.sh` is the check. It mirrors `.github/workflows/ci.yml` step for step and prints
`GATE GREEN` or `GATE RED` on its last line — read that line, not a scrollback of passes.

The first native configure fetches and builds a trimmed OpenCV from source (ADR 0047), which takes
minutes and is then cached in the build directory. Nothing else about the first run is slow.
