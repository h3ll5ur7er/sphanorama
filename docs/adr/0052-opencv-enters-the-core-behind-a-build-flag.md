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
`NullRegistrationEngine` is what a build without it gets.**

Said precisely, because an earlier draft of this sentence said "what the composition root gets
otherwise" and asserted a branch with no first arm: `bridge/runtime.h` holds a
`NullRegistrationEngine` unconditionally, and nothing outside the tests constructs the OpenCV one.
That is not an oversight to fix here. The only composition root in the repository is the WASM
runtime, where `SPHANORAMA_WITH_OPENCV` is off — a selection there would be dead code today, and the
native client that will make it live (`bench/`) does not exist yet. What is decided now is where the
dependency may be linked; which root selects it is decided when there is a root that could.

This is the shape `core/src/engines/` already uses: two implementations of one contract sharing a
directory, chosen at composition rather than by the caller. `pose_engine` and `coverage_planner_engine`
each do it today; this adds a build condition to that selection rather than a new pattern.

- **The engine layer is where the dependency stops.** No manager, no resource access and no
  contract type gains an OpenCV type. `FeatureSet` crosses as `FrameRef`s (ADR 0051), which the
  frame store already owns, so nothing about OpenCV's presence is visible above the engine.
- **The flag already exists and already means this.** `SPHANORAMA_WITH_OPENCV` is off for WASM and
  on natively; adding a second axis would be inventing configuration.

**And the two things ADR 0047 left for this ADR to settle, settled.**

***The exception boundary is built, and it narrows ADR 0006.*** That ADR's rule — `Result<T>`
everywhere, nothing thrown across a layer or the WASM boundary — is unchanged in everything it is
about; what changes is that one translation unit is now compiled with exceptions so it can convert
OpenCV's at its own edge. 0006 carries the reciprocal note. 0047 named the shape and deferred it: "when an engine calls OpenCV it
cannot be compiled `-fno-exceptions`, because `cv::Exception` is how OpenCV reports ordinary
failure… the OpenCV-backed implementation is its own component, compiled with exceptions, catching
at its own edge and returning `Result<T>`. That is a decision for the ADR that introduces the first
such engine." This is that ADR, and that is the shape taken: `feature_registration_engine.cpp` alone
carries `-fexceptions` (a source-file property in `core/CMakeLists.txt`, verified to land after the
target's `-fno-exceptions` and on no other translation unit), and `ExtractFeatures` converts
`cv::Exception` to a `Result` at its edge. It is not theoretical: a one-pixel frame passes every
guard the engine has and then throws out of `cv::resize` under ORB and `setSize` under AKAZE, while
SIFT answers normally — which is why the boundary cannot be a list of the detectors that need one.

***The sanitizer remedy 0047 named is not available, and this supersedes it.*** 0047 said the remedy
for OpenCV tripping the sanitizers would be "a suppressions file scoped to `_deps/opencv-src`, not
turning recovery back on". Those two cannot both hold. A UBSan suppressions file is consulted only
for *recoverable* errors, and this repository's preset is `-fno-sanitize-recover=all`. Measured
rather than reasoned — and the first version of this measurement was itself confounded, which is
worth leaving in. It compared exit codes: 1 with recovery off and a suppressions file, 0 with
recovery on and one. But a recoverable UBSan error exits 0 whether it is suppressed or not, so the
exit code was reporting the recovery setting and nothing about the suppression. The discriminating
question is whether the error is **reported**:

| | exit | reported? |
| --- | --- | --- |
| recovery off, no suppressions file | 1 | reported |
| recovery off, **with** suppressions file | 1 | **reported** |
| recovery on, no suppressions file | 0 | reported |
| recovery on, with suppressions file | 0 | silent |

The second row is the whole argument: with recovery off, adding the suppressions file changes
nothing, because it is never consulted. So the suppressions route requires exactly the thing 0047
refused. The compile-time flag reaches 0047's real goal —
OpenCV exempt, our own code strict, recovery still off everywhere — without that trade, and it is
narrower in one way too: it lifts one check rather than silencing one report.

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
- **A frame's geometry is still checked against a byte count, and that is a real limit this ADR
  accepts rather than closes.** Every guard in the core compares what a `FrameRef` *claims* against
  the size of the span `Pin` returns, because that is the only thing about the allocation any engine
  can see: `MemoryFrameStoreAccess::Entry` keeps a size and no geometry, and `IFrameStoreAccess` has
  no way to ask what a frame was allocated *as*. So a claim that is arithmetically consistent with
  the byte count is accepted however little it resembles the allocation. A reviewer demonstrated the
  sharp end: an honest I420 640x480 relabelled `{Gray8, 640, 719, stride 640}` passes both engines
  and reads 239 rows of chroma as picture — in bounds, so the sanitizers are silent, and what comes
  back is a wrong number rather than a crash.

  Four review rounds have now spent their worst findings on this one seam, in two engines that hold
  the same check by duplication. The structural answer is for the store to describe what it
  allocated — a `Describe`-style accessor on `IFrameStoreAccess`, against which a handle can be
  checked once instead of approximated in every engine. That is a contract change and its own ADR,
  and doing it inside this one would be a second architecture decision smuggled into a PR about
  feature extraction. Named here so the next person meets it as a known gap rather than as a
  surprise.

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
