# ADR 0013 — The boundary marshals a generated binary codec, not JSON or FlatBuffers

**Status:** accepted. Refines the FlatBuffers intent in `docs/04` §4.6.

## Context
Structured calls have to cross between the TypeScript client and the C++ core. `docs/04` said
FlatBuffers; ADR 0012 established that the transport is a C ABI over the shared heap. Neither
settled the encoding, and the encoding is where a boundary quietly breaks: if the two sides
disagree about field order or width, a message decodes into plausible-looking values rather than
failing.

Three options were live:

- **JSON.** Needs a C++ parser. Hand-writing one is ~200 lines of tokeniser in the layer where a
  bounds check either exists or does not; the obvious libraries throw, and exceptions are disabled
  in the core (ADR 0006).
- **FlatBuffers.** Solves it properly, and adds `flatc` to the toolchain, a second schema language
  alongside the C++ headers that are already the IDL (ADR 0009), and a generated C++ API that is
  not the plain structs the core uses.
- **A codec generated from the contracts.** Both halves emitted from one parse of the headers we
  already parse.

## Decision
Generate the codec. `tools/contract_gen.py` emits `contracts/cpp/sphanorama/codec.h` and
`shell/src/bridge/codec.generated.ts` from the same parse, so the two sides cannot disagree about
field order or widths — there is no second place to get it wrong.

The primitives underneath are **hand-written** in `sphanorama/wire.h` and `shell/src/bridge/wire.ts`.
That layer is small and fiddly, and generating it would mean reviewing generated bounds checks.
The reader never throws, never reads past the end, and stays failed once it has failed, so callers
check `ok` once rather than after every field.

Only types reachable from a `@boundary` method are emitted: nothing else crosses.

Encoding: little-endian throughout. `int64_t` crosses as a double (counts and timestamps, which
stay JS numbers), `uint64_t` as eight raw bytes (only ever a content hash, where the low bits are
exactly what decides whether a build stage is reused), enums as their index, ids as doubles,
strings and byte payloads length-prefixed.

## Consequences
- No new toolchain dependency and no second schema language.
- A golden payload — one hex constant asserted by both the C++ and the TypeScript suite — pins the
  two halves to each other rather than each to its own emitter. A disagreement fails a test
  instead of producing nonsense in a browser.
- Writing the emitters found two real defects. The mirror generator mapped `Status` to
  `Result<void>` unconditionally — right for a method return, wrong for a field — so
  `BuildProgress::failure` had silently lost its type; returns and fields are now mapped
  separately. And `Status::component` was a `std::string_view`, which encodes fine and *cannot be
  decoded*: the generated `Decode` assigned it a temporary, leaving a dangling view on every
  status that crossed the boundary. It is a `std::string` now, and the generator refuses a
  `string_view` data member outright so the shape cannot recur.
- The dangling view was caught by Clang through the Emscripten build, not by GCC and not by a
  test. Keeping both compilers in CI is what found it.
- Cost: a hand-rolled format with no schema evolution story. Acceptable while both sides ship
  together in one artefact; the moment a persisted document uses this encoding, versioning has to
  be designed rather than assumed.
- FlatBuffers is still the right answer for zero-copy over large payloads. Nothing here forecloses
  it — the generated codec is an implementation detail behind the facade, and pixels never cross
  this way in any case.

## Rejected
- *JSON.* A parser in the core, in the layer least forgiving of a missing bounds check, to send
  four doubles.
- *FlatBuffers now.* Real weight for a benefit that only matters once payloads are large, and it
  would put a second IDL beside the one ADR 0009 just established.
- *Hand-written codecs.* Exactly the drift the contract mirror exists to prevent, reintroduced one
  struct at a time.
