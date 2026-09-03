# Sphanorama

On-device photo-sphere capture PWA. C++20 core → WebAssembly, thin TypeScript shell, Python
tooling. Decomposed by volatility (iDesign).

**Read `.claude/skills/sphanorama-engineering/SKILL.md` before working here.** It carries the
layer rules, the TDD workflow, contract discipline, repo structure and the definition of done.
Rationale is in `docs/00-principles.md` and `docs/03-architecture.md`.

The four things that are most expensive to get wrong:

1. **Write the test first.** Correctness here is invisible to the eye and the target device is a
   phone. `docs/00-principles.md` §0.2 lists the invariants worth reaching for when the expected
   output isn't knowable in advance.
2. **Respect the layers.** Clients call managers only; managers never call managers; engines are
   stateless and touch only the compute and frame-store resource accesses. CI fails on a violating
   include edge.
3. **Update the docs in the same commit** as the change that invalidates them, and write an ADR for
   anything that adds a component, changes a contract, adds a dependency, or takes a rule exception.
4. **Before adding a component, name the volatility it absorbs.** If it's already in
   `docs/02-volatility-map.md`, extend the existing owner instead.

Status: Phase 0's exit criterion is met. The native and WASM builds, the PWA shell, the three
managers, the generated boundary (contracts mirror, wire codec, facade dispatch) and the Pages
deploy are in and green. A phone opens the app, the core plans a real tessellation for the lens the
page reports, and the reticle follows guidance that came back from `CaptureSessionManager` — pose,
coverage and acceptance are all decided in the core.

Three engines are still null (`FrameQuality`, `Registration`, `Composition`). The call that did not
fit the resident-port pattern, `ICameraAccess::CaptureBurst`, is gone: a burst takes time, so it is
now paced by `CaptureSessionManager` across the ticks the client already makes, over the preview
frame the page keeps resident (ADR 0018). `ArmBurst` arms one; `PeekPreviewFrame` is the whole pixel
path. The core itself runs in a dedicated module worker, with the camera and motion adapters left
in the page pushing frames and IMU batches across (ADR 0019). See `docs/06-roadmap.md`.
