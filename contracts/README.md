# Contracts

The interfaces *are* the architecture. They are written before implementations and reviewed as
prose; a change here is an architectural change, not a refactor.

`contracts/cpp` is the **include root** — the build puts it on the include path and the core
consumes these headers directly. They are never copied into `core/include`, because two copies of
an interface is exactly the drift problem the contract check exists to prevent (ADR 0008).

## Layout

One interface per header. That is not tidiness: the layer check works at header granularity, and
under an aggregate `managers.h` a manager *calling* another manager is indistinguishable from a
manager *implementing* its own interface.

```
cpp/sphanorama/
  types.h                    every value type, plus Status / Result<T> and SPH_TRY
  utilities/                 logger · clock · config_store · arena · diagnostics · event_bus
  engines/                   coverage_planner · pose · frame_quality · registration · composition
  managers/                  capture_session · panorama_build · project
  resource_access/           camera · motion_sensor · frame_store · project_store ·
                             image_codec · compute_device · export
ts/contracts.d.ts            the subset that crosses the WASM boundary
```

`types.h` holds **only** data and the interface headers hold **only** interfaces. Data has no
layer — `GlobalSolution` is produced by the registration engine and consumed by the composition
engine — so putting shared value types anywhere else would force an engine-to-engine dependency
that the layer rules forbid, for no reason other than where a struct was written down.

## Markers

The boundary has two directions, so an interface carries up to two markers:

- `// @boundary` — mirror this interface into TypeScript.
- `// @facade` — *also* generate dispatch for it, because the client calls into it.

Only managers carry `@facade`. Resource accesses are mirrored because the browser implements them,
and dispatching one would generate a call into a runtime that has no such thing.

## Rules

1. **C++ is the source of truth.** `ts/contracts.d.ts` is **generated** by
   `tools/contract_gen.py`, along with both halves of the wire codec and the facade dispatch —
   never edit it. Change the C++ header, run the generator, and commit the output with it;
   `contract_gen.py --check` fails CI on drift.
2. **No frames in a contract.** Frames cross as `FrameRef` handles. `IFrameStoreAccess::Pin` is
   the only way to reach a frame's bytes, and only inside the core. The one image that leaves is a
   `FramePreview` — reduced, bounded by `kFramePreviewMaxEdge`, and there because a page has no
   frame store to resolve a handle against. The rule is about cost, and the reduction is what pays
   it (ADR 0038).
3. **No exceptions.** Everything fallible returns `Result<T>`; a bare `return status;` propagates
   a failure out of any `Result<U>`-returning function, and `SPH_TRY` unwraps or propagates.
4. **Managers are the client's only surface.** If a client needs an engine, either the client is
   doing business logic, or a manager method is missing.
5. **Resource access is implemented twice** — TypeScript for the browser, native for the bench and
   tests — behind one contract. Anything that cannot be implemented natively is not a resource
   access; it is a browser detail that leaked.

Read [`../docs/03-architecture.md`](../docs/03-architecture.md) before changing anything here.
