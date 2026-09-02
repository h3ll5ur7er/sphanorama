# ADR 0002 — C++/WASM core, TypeScript resource-access adapters

**Status:** accepted

## Context
Business logic must be fast and must not be rewritten per platform. Camera and motion sensors are
only reachable from JavaScript. iDesign puts resource access below the business logic — but here
the resource lives on the other side of a language boundary.

## Decision
All managers, engines and resource-access **contracts** live in the C++ core. Browser
implementations of `ICameraAccess`, `IMotionSensorAccess`, `IProjectStoreAccess`,
`IImageCodecAccess`, `IComputeDeviceAccess` and `IExportAccess` are TypeScript adapters injected
into the core at startup through generated ports. Native implementations of the same contracts
back the bench client and the test suite.

Two invariants make it viable:
- **Control crosses the boundary; pixels do not.** Frames are written once into a shared heap and
  referred to by handle thereafter.
- **The boundary is generated from one IDL**, so a contract change is a compile error on both
  sides.

## Consequences
- The core compiles natively with zero Emscripten symbols — enforced in CI. This is what keeps
  browser assumptions out of business logic.
- Managers can be tested with a folder of images and a recorded IMU log standing in for the camera
  and sensors.
- Cost: a codegen step and an async proxy layer.

## Rejected
- *Managers in TypeScript, engines in WASM.* Sequencing logic is exactly the volatile part; it
  would then exist only in the browser and be untestable natively.
- *Everything in TypeScript.* Feature extraction, bundle adjustment and multi-band blending on
  mobile need SIMD and threads.
- *Rust core.* Equivalent on merit, but OpenCV is C++ and the binding cost is real.
