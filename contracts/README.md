# Contracts

The interfaces *are* the architecture. They are written before implementations and reviewed as
prose; a change here is an architectural change, not a refactor.

| File | Layer | Volatility absorbed |
| ---- | ----- | ------------------- |
| `cpp/sphanorama/types.h` | shared | — (pure data, generated to TS/FlatBuffers in Phase 0) |
| `cpp/sphanorama/managers.h` | Managers | V1 capture sequence · V2 build sequence · V3 project lifecycle |
| `cpp/sphanorama/engines.h` | Engines | V4 coverage · V5 pose · V6 selection · V7 registration · V8 composition |
| `cpp/sphanorama/resource_access.h` | ResourceAccess | V9–V15 camera, sensors, frame store, project store, codecs, compute, export |
| `cpp/sphanorama/utilities.h` | Utilities bar | logging, clock, config, arena, diagnostics, event bus |
| `ts/contracts.d.ts` | boundary mirror | the subset that crosses the WASM/JS boundary |

## Rules

1. **C++ is the source of truth.** `ts/contracts.d.ts` is currently hand-mirrored; from Phase 0 it
   is generated, and CI fails on drift.
2. **No pixels in a contract.** Frames cross as `FrameRef` handles. `IFrameStoreAccess.Pin` is the
   only way to reach bytes, and only inside the core.
3. **No exceptions.** Everything fallible returns `Result<T>`.
4. **Managers are the client's only surface.** If a client needs an engine, either the client is
   doing business logic, or a manager is missing a method.
5. **Resource access is implemented twice** — TypeScript for the browser, native for the bench and
   tests — behind one contract. Anything that cannot be implemented natively is not a resource
   access, it is a browser detail that leaked.

Read [`../docs/03-architecture.md`](../docs/03-architecture.md) before changing anything here.
