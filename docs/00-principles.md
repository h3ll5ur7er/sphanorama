# 0. Engineering principles

Read this before the architecture. The architecture says what the system is; this says how it gets
built, and the two are meant to constrain each other.

## 0.1 Three commitments

1. **Tests come first.** Not "we have good coverage" — the test is written before the thing it
   tests, and it fails for the right reason before it passes.
2. **Documentation and architecture are deliverables**, on the same footing as code. A change that
   invalidates a document updates that document in the same commit.
3. **The layer rules are load-bearing**, not advice. CI fails on a violating edge.

None of these are free, and each is here because of a specific way this project would otherwise go
wrong. The reasoning is below; if you disagree with the reasoning, change the principle by argument
rather than by exception.

## 0.2 Tests come first

### Why here specifically

Panorama stitching is a domain where you cannot tell correct from nearly-correct by looking. A
registration that is 0.4° off produces an image that looks fine until you inspect a seam; a gyro
bias that accumulates over 40 cells produces a sphere that closes with a visible tear only at the
very end. And the target device is a phone — the worst debugger available.

So the test is not a safety net bolted on afterwards. It is the only mechanism by which we know
what the code does. Writing it first is what forces the question *"what would correct even mean
here?"* to be answered before there is an implementation to rationalise around.

### What "test first" means, per layer

| Layer | The test you write first | Why this shape |
| ----- | ------------------------ | -------------- |
| **Engines** | A native GoogleTest with explicit inputs and an asserted output or invariant | They are pure functions by contract. There is no excuse available |
| **Managers** | A sequence test driving the real manager against **fake** resource accesses — a folder of frames and a recorded IMU log | This is what the resource-access contracts are *for*. If a manager is hard to test, a dependency has leaked past the contract |
| **ResourceAccess** | A shared contract-test suite, run against both the browser and the native implementation | The interesting property is that the two implementations agree, not that the browser works |
| **Boundary** | A round-trip test per value type: C++ → wire → TS → wire → C++ | Generated code, so these are cheap and catch drift immediately |
| **Clients** | Tests over the logic (which reticle, what guidance, what state), never the pixels | Visual output is reviewed by eye; the decisions behind it are not |

### "I can't write the test first, I don't know the right answer"

Usually false, and worth pushing on, because this is the sentence that kills TDD in numerical code.
You may not know the output, but you know the **invariant**. These are the ones worth encoding
here, and several of them are load-bearing for features:

- A quaternion round-tripped through a rotation matrix comes back unchanged.
- Every direction on the sphere falls inside at least one cell's acceptance cone — the coverage
  plan is complete by construction.
- Registering a frame against itself yields identity rotation with all points as inliers.
- Blending a single frame yields that frame.
- Exposure compensation over frames with equal exposure is a no-op.
- Selection is deterministic: the same candidates and policy produce the same ranking.
- **An incremental rebuild equals a full rebuild.** Invalidate any set of nodes, rebuild, and the
  output is bit-identical to building from scratch. This one test is the safety net under the
  entire retake feature, and it should exist before the incremental path does.

Where genuinely only accuracy matters, the test-first artefact is the **harness**: a synthetic
dataset with ground truth and an asserted error bound. Write the bound first, generously, then
tighten it as the implementation improves. A generous bound that exists beats a precise one that
doesn't.

### The honest exceptions

TDD is the default, not a ritual. It does not apply to:

- **Exploratory work.** Spiking an algorithm to find out whether an approach is viable is fine —
  the spike is thrown away, and the real implementation is written test-first afterwards. Say so
  in the commit message if a spike survives contact with reality.
- **Browser adapter plumbing.** A test that asserts `getUserMedia` works is testing the browser.
  Test the adaptation — format conversion, error mapping, capability reporting — and cover the rest
  with the end-to-end suite.
- **Visual output.** Nobody writes an assertion for whether a blend looks right. Cover it with a
  perceptual-difference check against a stored render so *changes* are caught, and review the image
  by eye.

## 0.3 Documentation and architecture are deliverables

### The same-commit rule

If a change makes a statement in `docs/` false, the fix ships in the same commit. Not the next one,
not a follow-up issue. A document that lags reality is worse than no document, because people
extend trust to it that it no longer earns — and the reader who gets burned stops trusting the
whole set.

This is cheap when the docs are structured so that a code change touches one section. If you find
yourself updating five documents for one change, that is a signal the documentation has the same
coupling problem the architecture is designed to avoid.

### When an ADR is required

Write one in `docs/adr/` when you:

- add or remove a component, or move responsibility between components;
- change a contract in `contracts/`;
- add a third-party dependency;
- take an exception to a layer rule;
- reverse or amend an earlier ADR — supersede it, never edit it into agreement with the present.
  The record of what we thought at the time is the point.

An ADR is four short sections: context, decision, consequences, and the alternative rejected and
why. The rejected alternative is the part that pays off later, when someone asks "why didn't we
just…".

### Contracts are prose

The headers in `contracts/` are read as the architecture, so write them for a reader: comments
explain *why* an interface is shaped the way it is, not what the method name already says. A
contract change is an architecture change and gets reviewed as one.

### What we don't document

Not everything deserves a document. Skip: restating what the code says, a doc that duplicates a
contract header, "how to run the tests" in five places, and status updates. If a document has no
job, deleting it is an improvement.

## 0.4 Definition of done

A change is done when all of these hold. They are cheap to check and expensive to skip:

- [ ] The test was written first and failed for the intended reason.
- [ ] The implementation is the smallest thing that passes it.
- [ ] The layer check passes — no violating call edge.
- [ ] The contract-drift check passes — the TypeScript mirror matches the C++ headers.
      *(Not yet implemented: the mirror is still hand-maintained until the Phase 0 codegen
      lands. Until then, update `contracts/ts/contracts.d.ts` by hand in the same commit.)*
- [ ] The native build compiles the core with zero Emscripten symbols.
- [ ] Affected documents are updated in this commit.
- [ ] An ADR exists if the change qualifies (§0.3).
- [ ] The WASM size budget still holds.

## 0.5 Adding something new

Before writing a component, work through this. Most proposed components are variations of an
existing one, and the difference between the two is the difference between an architecture and a
phone book.

1. **Which volatility does it absorb?** Name it. If it is already in
   [`02-volatility-map.md`](02-volatility-map.md), the owner exists — extend that component.
2. **If it is a new axis**, add it to the volatility map with an ADR. A genuinely new axis is
   uncommon and worth the paperwork.
3. **Which layer?** Sequence and state → manager. Stateless activity → engine. Adapting a resource
   → resource access. If it seems to be two of those, it is two components.
4. **Can a client already do this by sequencing existing managers?** Then it is a client change.

Symmetrically: if a feature request lands and *no* existing component can absorb it, the volatility
analysis missed an axis. That is real information — record it.

## 0.6 Repo structure

```
contracts/cpp/        the include root — headers are consumed directly, never mirrored
core/                 C++: managers, engines, resource-access implementations, native adapters
  src/{managers,engines,resource_access,utilities}/
  test/               GoogleTest — mirrors src/ one file per unit
bench/                native CLI client: runs the core on datasets, prints timings
shell/                TypeScript PWA
  src/clients/{capture,review}/
  src/access/         browser resource-access adapters
  src/bridge/         generated facade + worker plumbing
tools/                Python: codegen, dataset generation, scoring, the layer check
contracts/            IDL + interface headers — the source of truth
docs/                 principles, architecture, ADRs
```

Where a file goes follows from its layer, not its feature. Everything about coverage planning does
not live in a `coverage/` directory; the engine lives with engines, its test with tests, its
contract with contracts. Feature-shaped directories are how layer discipline erodes.

## 0.7 Commits

- One logical change per commit, with its tests and its doc updates.
- The message says *why*, not what — the diff already says what.
- A commit that only reformats or only renames stays separate, so review can skip it.
- Never commit a red test to be fixed later. If work is in progress, it is not a commit.
