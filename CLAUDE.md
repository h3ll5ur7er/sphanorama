# Sphanorama

On-device photo-sphere capture PWA. C++20 core → WebAssembly, thin TypeScript shell, Python
tooling. Decomposed by volatility (iDesign).

**Read `.claude/skills/sphanorama-engineering/SKILL.md` before working here.** It carries the
layer rules, the TDD workflow, contract discipline, repo structure and the definition of done.
Rationale is in `docs/00-principles.md` and `docs/03-architecture.md`.

**Review with `.claude/skills/sphanorama-review/SKILL.md` before opening a PR.** It spawns reviewer
subagents, one per lens, against the mistakes this codebase has actually made. Reviews are run here
rather than bought from a bot.

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

Two engines are still null (`Registration`, `Composition`). `FrameQuality` has a real one:
sharpness is the variance of a Laplacian over a downscaled luma plane, exposure agreement is
measured against the rest of the burst, and `Rank` normalises before it weights so the selection
policy's knobs all turn something. `motionBlur` stays zero and says why — smear in pixels needs an
exposure time and a focal length the engine is not handed. The call that did not
fit the resident-port pattern, `ICameraAccess::CaptureBurst`, is gone: a burst takes time, so it is
now paced by `CaptureSessionManager` across the ticks the client already makes, over the preview
frame the page keeps resident (ADR 0018). `ArmBurst` arms one; `PeekPreviewFrame` is the whole pixel
path, and it reaches the browser: the page draws the viewfinder into a canvas and transfers the
buffer to the worker the core runs in, where `PeekPreviewFrame` copies it into the frame store
(ADR 0019, ADR 0021). A burst armed from the capture loop captures real pixels, which an
end-to-end test drives in a real browser. Its frames share an exposure where the camera can hold
one: the page applies the locks and confirms them by reading the mode back before arming, and
`SetLocks` refuses a lock it has not been told is held (ADR 0022). The frame store's browser
ceiling is read from `navigator.deviceMemory` rather than stated, so it scales with the machine;
and the policy above it is in: `CaptureSessionManager` cools a cell the moment its burst is
ranked, so a sphere larger than the store still captures and `Allocate`'s refusal is the backstop
rather than the first thing a capture hits (ADR 0023). See `docs/06-roadmap.md`.
