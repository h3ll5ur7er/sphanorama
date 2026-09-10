# 0052 — OpenCV enters the core behind a build flag, and the browser gets a null registration

**Status:** accepted

## Context

ADR 0005 chose OpenCV and named six modules. ADR 0047 built it natively at a pinned commit and
deferred the WASM cross-compile against the size budget. What neither settled is where OpenCV is
allowed to be *linked*, because until now nothing needed it linked anywhere but a test.

Today `sphanorama_opencv` links into `sphanorama_tests` only (`core/test/CMakeLists.txt`), guarded
by `SPHANORAMA_WITH_OPENCV`, and its single consumer cross-checks `camera_model` against
`cv::projectPoints`. Nothing in `core/src` links it. `RegistrationEngine` is the first shipped core
code that would.

That forces a decision the roadmap has been able to postpone: the WASM build does not have OpenCV,
so a core that depends on it unconditionally does not compile for the browser at all.

## Decision

**The OpenCV registration engine is compiled only when `SPHANORAMA_WITH_OPENCV` is on.
`NullRegistrationEngine` is what the composition root gets otherwise.**

This is the shape `core/src/engines/` already uses: two implementations of one contract sharing a
directory, chosen at composition rather than by the caller. `pose_engine` and `coverage_planner_engine`
each do it today; this adds a build condition to that selection rather than a new pattern.

- **The engine layer is where the dependency stops.** No manager, no resource access and no
  contract type gains an OpenCV type. `FeatureSet` crosses as `FrameRef`s (ADR 0051), which the
  frame store already owns, so nothing about OpenCV's presence is visible above the engine.
- **The flag already exists and already means this.** `SPHANORAMA_WITH_OPENCV` is off for WASM and
  on natively; adding a second axis would be inventing configuration.

## Consequences

- **The browser ships a core that cannot stitch, and this makes that visible rather than latent.**
  It is true today — `RegistrationEngine` is null everywhere — but once a native path works, "the
  phone cannot do this yet" stops being a statement about unwritten code and becomes a statement
  about a build. The roadmap's Phase 2 exit criterion needs to say which build it is talking about.
- **The accuracy number is a native number first.** Phase 2's exit criterion asks for a median
  registration error on the synthetic dataset. That will be measured natively, and whether it
  transfers to a WASM build of the same code is a separate measurement nobody has taken. Assuming
  it transfers is exactly the kind of untested claim this project keeps catching.
- **Two build shapes now differ in behaviour, not just in size.** The no-browser checker and the
  layer check must keep passing with the engine present; the size budget only bites where the
  engine is absent. So neither build alone exercises the whole tree, and CI has to run both — which
  it already does.
- **Linking OpenCV into the core is what first runs its SIMD paths under the sanitizers, and one
  check had to be lifted for it.** `cv::ORB::detectAndCompute` resizes to build its pyramid, and
  OpenCV's bit-exact 8-bit resize gathers pairs of source pixels through
  `*(const short*)(row + index)`. The index is a *pixel* offset into an 8-bit row, so that address is
  odd for half of all inputs whatever the row's base alignment — it is not something a different
  allocation could fix, and the read is in bounds, so ASan says nothing and only
  `-fsanitize=alignment` objects. `cmake/opencv.cmake` therefore appends `-fno-sanitize=alignment`
  around the `add_subdirectory` that pulls OpenCV in, and restores the flags afterwards. The scoping
  is the point and was verified in both directions: the flag is present on `resize.cpp`'s command
  line and absent from ours, and a deliberately misaligned load written into
  `feature_registration_engine.cpp` still aborts the run naming that file. Every other UBSan check,
  and all of ASan, still cover OpenCV. The cost is that a genuine alignment fault inside OpenCV would
  now go unreported, which is accepted because we do not write OpenCV's SIMD kernels.
- **A conditional engine is a conditional test.** The registration tests exist only under the flag,
  like the OpenCV cross-check before them. A reader looking for registration coverage in a WASM-only
  checkout will find none, and the CMake comment says so where they will be looking.

## Rejected

***Cross-compiling OpenCV to WASM now, so there is one build.*** Honest and expensive: ADR 0047
deferred it on the size budget, and nothing has changed except that we now want it more. It is worth
doing when there is a measured registration path to justify the bytes — which is after this, not
before.

***Putting the OpenCV work behind a resource access, so the core stays pure.*** Superficially
attractive because it would make the dependency swappable. Rejected because it is a lie about the
layers: feature extraction is a stateless activity over pixels, which is the definition of an engine
here, and calling it a resource would mean the browser has to implement it. A resource access is a
thing the platform provides; OpenCV is a library we chose.

***Shipping the native engine and letting the WASM build fail to link.*** Would at least be loud.
Rejected because the browser is the product's actual target, and a core that does not build for it
is not a core that is nearly done — it is one that has stopped being buildable while it waits.
