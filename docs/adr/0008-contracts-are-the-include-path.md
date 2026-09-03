# ADR 0008 — One interface per header, and contracts are the include path

**Status:** accepted

## Context
Two problems surfaced the moment Phase 0 started building the layer check that ADR 0007 promised.

First, the layer rule that matters most — *managers never call other managers* — was not checkable.
Every manager implementation includes the header declaring its own interface, and under an
aggregate `managers.h` that include is indistinguishable from one manager reaching for another.
The rule would have been enforced in name only.

Second, `docs/05` had `core/include/sphanorama/` mirroring `contracts/`. Two copies of an
interface, kept in step by hand, is the same drift problem the contract check exists to prevent —
introduced by the very structure meant to prevent it.

## Decision
- **One interface per header**, grouped by layer:
  `contracts/cpp/sphanorama/{utilities,engines,managers,resource_access}/<component>.h`.
- **`contracts/cpp` is the include root.** The build adds it to the include path; nothing is
  mirrored into `core/`.
- **`types.h` holds every value type and no interfaces**; interface headers hold interfaces and no
  value types. Data has no layer, so shared types (`GlobalSolution`, `FeatureSet`, `CameraCapabilities`)
  live there rather than forcing an engine-to-engine include.
- Component identity comes from the path, so a component's private headers live in a directory
  named for it (`core/src/engines/pose_engine/detail.h`). Otherwise two components sharing a
  directory could couple through a relative include and never touch each other's contract.
- `Result<T>` gains a converting constructor from `Status` plus `Ok`/`Err`/`Fail`/`SPH_TRY`.
  Without ergonomics at least as cheap as the alternative, a no-exceptions rule decays into
  out-parameters and sentinel values.

## Consequences
- The layer check is real: `tools/layer_check.py` distinguishes a component depending on itself
  from one depending on a sibling, and 23 tests pin the matrix, including the two sanctioned
  engine exceptions.
- Contract headers get read one interface at a time, which is how they are reviewed anyway.
- 24 header files instead of 5. Accepted: they are small, and each is now a unit of review.
- Value types concentrate in one large `types.h`. Watch it — if it becomes hard to navigate, split
  it into `types/` with an umbrella header rather than pushing types back into interface headers.

## Rejected
- *Aggregate headers plus a real C++ parser* (libclang) to check at symbol granularity. More
  precise, and it makes the check slow, fragile across compiler versions, and hard to run in a
  pre-commit hook. Header granularity buys nearly all the enforcement for a fraction of the cost —
  and it costs one file split, which improves the contracts anyway.
- *Mirroring contracts into `core/include`* to keep the core self-contained. It buys nothing the
  include path does not, and pays for it in drift.
