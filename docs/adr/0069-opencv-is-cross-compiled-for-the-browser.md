# 0069 — OpenCV is cross-compiled for the browser, and the registration table is measured there

**Status:** accepted; supersedes in part [ADR 0047](0047-opencv-is-fetched-pinned-and-trimmed-and-earns-its-place-on-a-cross-check.md)
(OpenCV linked natively only) and [ADR 0052](0052-opencv-enters-the-core-behind-a-build-flag.md)
(the flag off for WebAssembly)

## Context

ADR 0047 built a trimmed OpenCV natively and deferred the WebAssembly cross-compile, which "has its
own size budget, its own SIMD questions, and its own failure modes". ADR 0052 made the registration
engine compile only where OpenCV does, so every browser build got `NullRegistrationEngine`. Both
were right to wait: nothing needed registration in a browser, and the algorithms could be written
and measured natively first.

That is no longer true. Phase 2 exits on a capture a phone stitches, the registration engine now
solves a ring to hundredths of a degree natively, and the next increment wires it into
`PanoramaBuildManager` — which runs in the browser. Whether OpenCV fits the 8 MB budget, whether its
exceptions survive the cross-compile, and whether the browser's build registers as well as the
native one were three open questions with no numbers behind them.

A spike answered them before this was decided, and its figures are below. It was thrown away; what
follows was written again from the start.

## Decision

1. **OpenCV is on in every build that has a reason for it, WebAssembly included.**
   `SPHANORAMA_WITH_OPENCV` defaults on everywhere, and the WebAssembly presets set it explicitly,
   so a build directory whose cache still says off from the old default is overridden at the next
   configure rather than left silently without it. `native-contracting` keeps it off, for the reason
   it gives.
2. **WebAssembly exception handling** (`-fwasm-exceptions`), compiled everywhere and linked by
   whatever links the registration engine, which is a library of its own so that its interface can
   carry the flag: the harness below today, the module when it first calls the engine. The compile
   flag changes nothing in a translation unit built `-fno-exceptions`; the link flag brings in an
   exception-handling runtime, which is not free. And it cannot be left to each executable to ask
   for: one linked without it links cleanly, and the first throw aborts it. Emscripten 6.0.9 emits the
   legacy encoding, which needs Chrome 95, Firefox 100 and Safari 15.2, where the 128-bit SIMD the
   module already requires needs Chrome 91, Firefox 89 and Safari 16.4. So the floor moves on Chrome
   and Firefox, by versions from 2021 and 2022 that phones have long updated past, and not on Safari
   — and it moves when the module first links the runtime, not in this change. ADR 0006 is
   untouched: nothing throws across a layer or the WebAssembly boundary; one engine converts at its
   own edge, now in both builds.
3. **One instruction set and no dispatch.** Under Emscripten, OpenCV's CPU baseline and dispatch
   lists are empty and its universal intrinsics compile to WebAssembly SIMD, which every preset asks
   for. Its thread pool follows the build: the threaded preset compiles `-pthread` and gets OpenCV's
   pthreads backend, the single-threaded one gets none. Which build is which is asked of the
   compiler (`__EMSCRIPTEN_PTHREADS__`) rather than read off the flags' spelling, which misses a
   `-pthread` from anywhere but `CMAKE_CXX_FLAGS`.
4. **The registration table is measured in WebAssembly.** `registration_accuracy_wasm.cpp` solves
   the photograph ring as `TheRingSolvedWithItsClosingPairIsWithinTheStatedBound` does — the same
   ring, solve, bounds and facing check, all from `support/solved_ring.h` — compiled for WebAssembly
   and run under node, for both builds. The runner reports the ring's shape (`--ring`) and refuses a
   dataset of any other, so the script that renders it holds no copy. `tools/wasm_accuracy.sh`
   renders and runs it; the gate and CI call that script, and it fails a run that measured fewer
   detectors than the binary has, or whose answers are not the numbers it asked for.

## Consequences

- **The size, measured.** Gzipped, on the release presets:

  | | single-threaded | threaded |
  | --- | --- | --- |
  | the module, which calls no registration | 98,935 | 112,134 |
  | a probe calling every registration method, null engine | 78,470 | — |
  | the same probe, OpenCV engine, all three detectors | 605,431 | 635,956 |

  The two probes were linked alike, with the exception flag, so the 527 KB between them is the
  engine and OpenCV. The exception-handling runtime is on top of that, and measured on the module by
  linking it with and without the flag it is about 29 KB (28,961 and 28,969 on two relinks): about
  556 KB of the 8 MB budget in all, when the module first calls the engine. These are `gzip -9` of
  the file; `size_budget.py`, which CI enforces, compresses its own way and reads 98,924 against
  98,928 on main. It does not grow yet (98,935 against 98,953 by `gzip -9`, four bytes fewer by the
  budget's reckoning), because nothing in it reaches the engine: `bridge/runtime.h` still holds
  `NullRegistrationEngine`, the linker drops what is unreachable, and the module does not link the
  exception runtime until it needs one. **All of it is at `-O3`, not the `-Oz` the presets ask
  for**: CMake's Release flags come after the preset's and the last level wins, for the core as for
  OpenCV. That predates this ADR and is its own change, which will move every figure here. **The size budget cannot see OpenCV until
  `PanoramaBuildManager` calls it**, which is when the table above becomes the module's.
- **The accuracy, measured, and it is not bit-exact.** Solving the ring, median and worst frame in
  degrees:

  | | native | WebAssembly |
  | --- | --- | --- |
  | ORB | 0.0464 / 0.1408 | 0.0464 / 0.1409 |
  | AKAZE | 0.0293 / 0.0660 | 0.0291 / 0.0662 |
  | SIFT | 0.0142 / 0.0331 | 0.0142 / 0.0331 |

  The two WebAssembly builds agree with each other to every printed digit. Against native, SIFT
  agrees in every figure, ORB in its median with its worst frame a ten-thousandth further out, and
  AKAZE in the fourth decimal and in its fitted focal length (492.744 against 492.766). Floating
  point is not bit-exact across instruction sets — AKAZE's diffusion most visibly, and some ORB path
  enough to move one frame. That is the reason the WebAssembly figures are measured rather than
  inferred from the native ones, and why they are held to the bounds rather than to the native
  figures.
- **The speed, from the spike, whose flags these presets reproduce**, under node on the machine that
  measured the rest, extracting features from twelve 640 by 480 frames — against native with OpenCV
  held to one thread, so the comparison is of instruction sets rather than of thread counts:

  | | native, one thread | WebAssembly | WebAssembly, threaded |
  | --- | --- | --- | --- |
  | ORB | 62 ms | 155 ms | 153 ms |
  | AKAZE | 510 ms | 720 ms | 470 ms |
  | SIFT | 894 ms | 940 ms | 574 ms |

  Twelve pairs take 55 to 100 ms and the solve 38 to 80. **A phone is not this machine**, and the
  number the exit criterion asks for — build time per device class — is still not measured. What
  this says is that nothing here is an order of magnitude from usable.
- **OpenCV's unaligned lookup-table loads are not a WebAssembly problem.** The gather that needs
  `-fno-sanitize=alignment` natively (ADR 0052) is legal WebAssembly, whose loads carry an alignment
  hint an engine may not fault on. Measured: the ring registers under `-sSAFE_HEAP=1`, which the
  `wasm-debug` preset links, about eight times slower and otherwise identically.
- **The exception boundary is exercised in WebAssembly, not assumed.** The harness hands each
  detector a one-pixel frame first — the case ADR 0052 found, where ORB throws out of `resize` and
  AKAZE out of `setSize` — and requires OpenCV's own text back as `Internal`, as the native test
  does. Replacing the engine's handler with one that answers `Ok` fails it for both.
- **OpenCV moves executables.** It caches `EXECUTABLE_OUTPUT_PATH` as `bin/`, which is why the
  native test binaries have always lived there; under Emscripten it moved the module out of
  `bridge/`, where the size budget, the shell, the deploy and the browser tests look for it. The
  module now names its own output directory.
- **The engine is compiled by clang now, and clang found three things gcc never has.**
  `-Wconversion` in clang includes sign conversion, and three `int` to `size_t` products in
  `FeatureRegistrationEngine::Extract` stopped the first cross-compile. None could be negative; they
  are cast now. The WebAssembly build is the first clang compile of the engine —
  `native-contracting` is clang but has OpenCV off — and CI now makes it on every push.
- **CI builds OpenCV twice more**, once per WebAssembly preset, each cached as the native builds are
  and keyed on emcc's version as well as the pin and the presets, since the presets' flags compile
  OpenCV here. Every one of those caches rebuilt most of OpenCV on a hit until this change, the
  native ones included, because they held neither `.ninja_deps` nor the headers configure generates
  beside `_deps`. The deploy builds only the module, which links none of it, and restores CI's cache
  without saving one of its own.

## Rejected alternatives

***Emscripten's JavaScript-emulated exceptions*** (`-fexceptions`). They need no browser support at
all, and work: the harness passes under them, the one-pixel throw included, and they would keep the
browser floor where SIMD puts it — Chrome 91 and Firefox 89 rather than 95 and 100. That is what
they buy, and it is four Chrome versions from 2021 and eleven Firefox versions to 2022, paid for on
every device, since every call that might throw becomes a trip through JavaScript. Measured on the
harness binary as it was before the ring and facing checks were added to it, which links everything
registration does: 661,059 bytes gzipped against 624,930 (5.8% more), glue 22,223 against 19,997,
and a full run under node 3.80 to 3.90 s against 3.45 to 3.77. And they are fussier about mixing:
asking for catching on a translation unit built `-fno-exceptions` is a hard error, where
WebAssembly's handling leaves such a unit alone.

***OpenCV's own `opencv.js` build.*** It is the official WebAssembly OpenCV, and it is a JavaScript
API over embind: the engine would call it through the boundary this architecture keeps pixels from
crossing, it bundles modules ADR 0005 excluded, and it is not the pinned, trimmed source the native
tests measure — so a native figure would say nothing about it.

***Registering on a server.*** Removes every question above and the reason the project exists: the
scope says everything is on-device, with no cloud stitch fallback (`docs/01-scope.md`, G5), and a
panorama that needs a network to stitch is not one.
