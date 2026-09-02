# ADR 0009 — The C++ header is the IDL

**Status:** accepted. Supersedes the codegen sketch in `docs/04` §4.6.

## Context
`docs/04` §4.6 said value types would be declared once in a neutral IDL and generated into C++
structs and TypeScript interfaces. `contracts/README` said, in the same breath, that C++ is the
source of truth. Both could not be true, and the conflict surfaced the moment the generator had to
be written.

The problem being solved is narrow: `contracts/ts/contracts.d.ts` is hand-maintained, and a
hand-maintained mirror of 344 lines of value types drifts. When it drifts, marshalling breaks in a
way that type-checks on both sides.

## Decision
The C++ contract headers are the IDL. `tools/contract_gen.py` parses them and emits the TypeScript
mirror; CI regenerates and fails on any diff.

The parser accepts only a small declared subset — scalar types, enums, `Id<Tag>` aliases, plain
data structs, and pure-virtual methods with named parameters — and **raises on anything else**.
Strictness is the mechanism, not an inconvenience: a lenient parser would silently drop a
construct it did not understand, and the mirror would then drift precisely where nobody was
looking. The subset cannot erode, because eroding it fails the build.

Only interfaces marked `// @boundary` are mirrored. Engines and the utilities bar never cross into
JavaScript, and three resource accesses — motion sensor, frame store, compute device — are also
excluded: their signatures move bytes through the shared WASM heap rather than through marshalled
values, so a generated TypeScript signature would describe a call that does not exist. Their
adapters are written against the shared-heap protocol instead.

## Consequences
- One artefact, not two: people edit the file they read, and the mirror cannot be edited at all.
- Writing the parser immediately found two constructs that could not cross the boundary and were
  fixed rather than special-cased: `Status::component` was a `const char*` (now `std::string_view`),
  and `EncodeSpec` carried a nested `enum class Format` (now a top-level `EncodeFormat`). A neutral
  IDL would have hidden both behind a translation layer.
- Contract methods must name their parameters. This is a real gain: an unnamed parameter documents
  nothing, and the generator refuses to guess.
- Doc comments propagate into the mirror, so the TypeScript carries the same rationale as the C++.
- Cost: a hand-rolled parser for a subset of C++, roughly 300 lines, that must be extended
  whenever the subset legitimately needs to grow. Accepted — extending it is a visible, tested
  change, which is the point.
- Byte sequences map to `Uint8Array` rather than `number[]`; mirroring an encoded image as an
  array of JS numbers would box one value per byte.

## Rejected
- *A neutral IDL generating both sides.* Cleanest in principle, and it means porting every existing
  type, splitting what you read from what you edit, and adding a language to learn. The drift it
  prevents is the same drift a strict parser prevents.
- *libclang.* Precise, and it makes the check slow, version-sensitive, and awkward to run in a
  pre-commit hook — the same trade rejected in ADR 0008 for the layer check.
- *Generating the C++ from the TypeScript.* The core is where the architecture lives; the browser
  is one client of it.
