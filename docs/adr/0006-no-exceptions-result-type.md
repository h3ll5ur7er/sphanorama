# ADR 0006 — `Result<T>` everywhere; no exceptions across layers or the WASM boundary

**Status:** accepted; narrowed by
[ADR 0052](0052-opencv-enters-the-core-behind-a-build-flag.md)

> **One translation unit is compiled with exceptions, and this ADR's rule is otherwise unchanged.**
> `feature_registration_engine.cpp` calls OpenCV, which reports ordinary failure by throwing
> `cv::Exception`, so it takes `-fexceptions` back and converts at its own edge. Nothing above it
> learns that OpenCV throws, no contract changes shape, and no exception crosses a layer or the WASM
> boundary — which are the three things this ADR is actually about. ADR 0047 named the shape and ADR
> 0052 built it. A second such file would need the same justification and its own line in
> `core/CMakeLists.txt`, where the exemption is granted by name rather than by pattern.

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
