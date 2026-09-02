# Sphanorama

An on-device photo sphere capture and stitching PWA. Open it in a phone browser, walk the
reticles around you, and get a full equirectangular panorama — with burst capture per reticle,
per-region retakes to kill parallax ghosts, and **no pixel ever leaving the device**.

This repository currently contains **architecture only**. No implementation has been written yet;
the first milestone is deliberately the architecture itself, following the
[iDesign Method](https://www.idesign.net) (decomposition by volatility, layered service map,
contracts defined before code).

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
  features, geometry and blending. Holds all Managers, Engines and ResourceAccess *contracts*.
- **Shell** — a thin TypeScript PWA. Camera, motion sensors, storage, and the capture UI. Supplies
  concrete ResourceAccess adapters to the core; contains no business logic.
- **Tooling** — Python for contract codegen, synthetic dataset generation, and offline
  ground-truth comparison.
