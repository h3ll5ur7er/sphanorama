# ADR 0007 — Tests and documentation are gated, not left to discipline

**Status:** accepted

## Context
This project has three properties that make good intentions insufficient. Correctness is invisible
to the eye — a registration 0.4° off looks fine until you inspect a seam. The target device is a
phone, the worst debugger available. And the architecture's value depends entirely on layer
discipline that nothing in C++ enforces.

Every one of those degrades silently. Nobody notices the day the docs stop matching the code, or
the day an engine starts holding state; they notice six months later when a change that should have
been local touches nine files.

## Decision
Make the practices checkable rather than aspirational:

- **Tests are written before the code.** The default is TDD, with named exceptions (spikes, browser
  plumbing, visual output) rather than a general escape hatch. Where the expected output is not
  knowable in advance, the first artefact is an invariant or an accuracy harness with an asserted
  bound — see `docs/00-principles.md` §0.2.
- **Documentation ships in the commit that invalidates it.** ADRs are required for a fixed list of
  change types, and superseded rather than edited.
- **CI enforces what review would otherwise have to catch**: the layer/include graph, contract
  drift between the C++ headers and the TypeScript mirror, a native build with zero Emscripten
  symbols, and the WASM size budget.
- The same rules are packaged as a project skill so that agent sessions in this repo inherit them
  rather than rediscovering them.

## Consequences
- A violating call edge or a stale generated mirror fails the build, so the architecture stays the
  thing that is actually there rather than the thing that was once drawn.
- The manager fakes and synthetic datasets that TDD requires are the same machinery the accuracy
  suite needs, so the cost is paid once.
- Cost: CI is slower, and some changes carry an ADR that feels heavy at the time. Accepted — the
  alternative degrades invisibly, which is the failure mode this project can least afford.

## Rejected
- *Convention plus code review.* It works while the codebase has one attentive author and stops
  working exactly when it matters.
- *Coverage thresholds instead of test-first.* Coverage measures which lines ran, not whether
  anyone decided in advance what correct meant. It is satisfiable by tests written to fit the
  implementation, which is the failure this is meant to prevent.
