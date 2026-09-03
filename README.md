# Sphanorama

An on-device photo sphere capture and stitching PWA. Open it in a phone browser, walk the
reticles around you, and get a full equirectangular panorama — with burst capture per reticle,
per-region retakes to kill parallax ghosts, and **no pixel ever leaving the device**.

Phase 0 is complete: the native and WASM builds, the generated boundary, the three managers, a real
coverage planner and pose engine, and the Pages deploy are in and green. A phone opens the app and
the reticle it sees is a coverage plan the core built and guidance the core returned. Capture is
not wired to pixels yet — see [docs/06-roadmap.md](docs/06-roadmap.md) for what is missing and why.

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

- **Core** — C++20 compiled to WebAssembly (SIMD + threads) via Emscripten, using OpenCV for
  features, geometry and blending from Phase 2 on. Holds all Managers, Engines and ResourceAccess *contracts*.
- **Shell** — a thin TypeScript PWA. Camera, motion sensors, storage, and the capture UI. Supplies
  concrete ResourceAccess adapters to the core; contains no business logic.
- **Tooling** — Python for contract codegen and the architecture checks CI runs; synthetic dataset
  generation and offline ground-truth comparison arrive with Phase 1's accuracy harness.
