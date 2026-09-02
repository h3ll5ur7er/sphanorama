# ADR 0012 — The WASM boundary is a C ABI over the shared heap, not Embind

**Status:** accepted

## Context
The first boundary function was written with Embind, which is the obvious choice: it binds C++
types to JavaScript objects with a few lines of declaration.

It did not work here. The core builds without RTTI (ADR 0008, for binary size), and Embind's type
registry is built on `typeid`. Emscripten offers `-DEMSCRIPTEN_HAS_UNBOUND_TYPE_NAMES=0` for that
case; it compiles, and then fails at runtime with an unbound type — where the diagnostic itself
reads out of bounds and crashes the renderer, because the error path is the thing that was
compiled out. The same pattern in isolation works under identical flags, so the trigger is
something structural in this project rather than the flags themselves.

That is the immediate cause. The deeper one is that Embind was the wrong mechanism regardless.

## Decision
Export a plain C ABI: `extern "C"` functions marked `EMSCRIPTEN_KEEPALIVE`, taking and returning
scalars and heap offsets, with structured results written into caller-allocated buffers the
JavaScript side reads through `HEAP32`.

## Consequences
- The boundary now uses **one** marshalling mechanism instead of two. `docs/04` §4.6 already
  commits to the shared heap and FlatBuffers: frames cross as offsets, and structured calls will
  cross as generated FlatBuffers. Embind value-objects would have been a second, hand-written path
  alongside the generated one, disagreeing with it in exactly the places nobody looks.
- No RTTI requirement, so ADR 0008 stands unamended.
- Smaller: dropping Embind took the module from 12,918 to 7,167 bytes uncompressed, before any
  real code exists to dominate that number.
- Field order across the boundary becomes a contract. It is declared once as an enum in
  `bridge/module.cpp` and read positionally on the JavaScript side; when the generated facade
  lands, that decoding moves into generated code rather than staying hand-written.
- Cost: more boilerplate per function than Embind, and manual `_malloc`/`_free` around output
  buffers. Acceptable — the surface is narrow by design, and most of it will be generated.

## Rejected
- *Enable RTTI everywhere so Embind works.* It pays binary size on every phone to make one
  registration mechanism work, and still leaves two marshalling paths.
- *Build only the bridge with RTTI.* Mixing RTTI across translation units that share polymorphic
  types is the ABI hazard the sanitizer build already caught once in this repo.
- *WebIDL binder.* Same duplication as Embind, plus a second IDL alongside the one in ADR 0009.
