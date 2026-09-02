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

Status: Phase 0 nearly complete. The native and WASM builds, the PWA shell, the three managers,
five null engines, the generated boundary (contracts mirror, wire codec, facade dispatch) and the
Pages deploy are in and green. The remaining structural piece is the resource-access ports, so a
capture session currently refuses with `CameraUnavailable`. See `docs/06-roadmap.md`.
