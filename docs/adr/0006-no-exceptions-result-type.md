# ADR 0006 — `Result<T>` everywhere; no exceptions across layers or the WASM boundary

**Status:** accepted

## Context
Emscripten exception support costs binary size and speed, exceptions do not cross into JavaScript
usefully, and the failures that matter here are expected conditions rather than bugs: permission
denied, out of storage, registration failed, not enough coverage.

## Decision
Every fallible call returns `Result<T>` carrying a `Status { code, component, detail }`. `StatusCode`
is a closed enum mirrored into TypeScript so clients branch on `SensorPermissionDenied` or
`FrameStoreExhausted` specifically. Build with exceptions disabled in the core.

## Consequences
- Failure handling is visible in the contracts, which is where the client needs to see it.
- Smaller, faster WASM.
- Cost: verbosity. Mitigated with a `TRY(...)` macro in the core.

## Rejected
*C++ exceptions with a translation layer at the facade.* It hides recoverable conditions from the
type system and pays the size cost anyway.
