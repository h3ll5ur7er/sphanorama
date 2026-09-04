---
name: sphanorama-engineering
description: Engineering principles, architecture rules, TDD workflow and repo structure for the Sphanorama photo-sphere codebase. Use this skill whenever you are writing, reviewing, planning or reorganising anything in this repository — code, tests, contracts, docs, ADRs, or commits — and especially before adding a component, changing an interface in contracts/, deciding where a new file belongs, or starting a feature. Also use it when asked where something should live, whether a change needs an ADR, why a layer rule exists, or how to test a piece of this system. Consult it before writing the first line, not after, since most of its value is in decisions that are expensive to reverse once code exists.
---

# Sphanorama engineering

This codebase is an on-device photo-sphere capture PWA: a C++20 core compiled to WebAssembly, a
thin TypeScript PWA shell, Python tooling. It is decomposed by volatility following the iDesign
method. Full rationale lives in `docs/`; this is the working reference.

Three things are non-negotiable here, and each exists because of a specific failure mode:

1. **Tests come first.** You cannot tell a correct stitch from a nearly-correct one by looking, and
   the target device is a phone — the worst debugger available.
2. **Docs and architecture are deliverables.** A document that lags reality is worse than none,
   because people trust it.
3. **Layer rules are enforced in CI**, not merely documented.

## Before writing anything: where does this belong?

Most proposed components are variations of an existing one. Work through this first — it is the
difference between an architecture and a phone book.

1. **Which volatility does it absorb?** Name it in one sentence. If that axis is already in
   `docs/02-volatility-map.md`, the owner exists — extend that component instead of adding one.
2. **New axis?** Add it to the volatility map, with an ADR. Genuinely new axes are uncommon.
3. **Which layer?**
   - Sequence, state, orchestration → **manager** (there are three; a fourth needs a strong case)
   - Stateless activity, an algorithm → **engine**
   - Adapting a device, store or API → **resource access**
   - Presentation and user intent → **client**
   - If it looks like two of these, it is two components.
4. **Could a client do this by sequencing existing managers?** Then it is a client change.

If a feature genuinely fits nowhere, the volatility analysis missed an axis. That is real
information — say so rather than forcing it into the nearest component.

## The layer rules

```
Clients  →  Managers  →  Engines
                      →  ResourceAccess  →  Resources
```

- Clients call **managers only** — never an engine, never a resource access. A client reaching past
  a manager means either the client is doing business logic, or a manager method is missing.
- Managers call engines, resource accesses and utilities. They may skip the engine layer.
- Managers **never call other managers.** Where a use case spans two, the client sequences it, or
  one publishes on the utility-bar event bus.
- Engines are **stateless per session**, never call managers, never call each other, and touch only
  two resource accesses: `IComputeDeviceAccess` and `IFrameStoreAccess`. Everything else arrives as
  a function argument. (Compute placement and pixel residency are properties of the device, not of
  the algorithm — threading them through every signature would invert the dependency for nothing.)
- Resource access talks to resources only. No policy, no cross-resource orchestration.
- Nothing calls upward. Progress flows back as return values or events.

A CI step parses the include graph and fails on a violating edge. If you need an exception, that is
an ADR, not a comment.

## Contracts

`contracts/cpp/sphanorama/` is the source of truth and is the include root — the build adds
`contracts/cpp` to the include path, so headers are consumed directly and never mirrored into
`core/`. One interface per header, which is what makes the layer check able to tell a manager
implementing its own contract from a manager calling another one (ADR 0008).

`contracts/ts/contracts.d.ts` is **generated** from those headers by `tools/contract_gen.py`
(ADR 0009) and must never be edited: change the C++ and regenerate. The parser accepts only a
small subset and raises on anything else, so a header that grows an unmirrorable construct fails
the build rather than drifting silently. Two markers, because the boundary has two directions: `// @boundary` mirrors an interface into
TypeScript, and `// @facade` additionally generates dispatch for it. Only managers carry
`@facade` — the browser *implements* the resource accesses, so those calls go the other way.
Engines and utilities never cross at all, and nor do the three resource accesses that move bytes
through the shared heap.

A `std::string_view` cannot be a data member of a contract type: it encodes fine and cannot be
decoded, since there is nothing owning the bytes. The generator refuses it.

- **No pixels in a contract.** Frames cross as `FrameRef` handles. `IFrameStoreAccess::Pin` is the
  only route to bytes, and only inside the core.
- **No exceptions.** Everything fallible returns `Result<T>` with a closed `StatusCode` enum, so a
  client can branch on `SensorPermissionDenied` specifically. Exceptions are disabled in the core.
- **Managers are the client's entire surface.**
- **Resource access is implemented twice** — TypeScript for the browser, native for the bench and
  tests — behind one contract. Anything that cannot be implemented natively is not a resource
  access; it is a browser detail that leaked.
- Header comments explain *why* an interface is shaped as it is. These headers are read as the
  architecture, so write them for a reader.

Changing a contract is an architecture change: it needs an ADR and is reviewed as prose.

## TDD in this codebase

Write the test first. It fails for the intended reason, then you make it pass, then you clean up.

### What the first test looks like, per layer

| Layer | Write this first | Why |
| ----- | ---------------- | --- |
| Engine | Native GoogleTest with explicit inputs and an asserted output or invariant | Pure functions by contract — no excuse available |
| Manager | Sequence test against **fake** resource accesses (a folder of frames, a recorded IMU log) | This is what the contracts are for. A manager that is hard to test has a leaked dependency |
| ResourceAccess | Add a case to the shared contract suite in `core/test/resource_access/`, then a fake in `core/test/support/` | The property that matters is that every implementation agrees; adding one is a line in the suite's type list (ADR 0010) |
| Boundary | Round-trip per value type: C++ → wire → TS → wire → C++ | Generated, so cheap, and catches drift immediately |
| Client | Tests over decisions — which reticle, what guidance, what state — never pixels | Visual output is reviewed by eye; the logic behind it is not |

### "I don't know the right answer yet"

This is the sentence that kills TDD in numerical code, and it is usually false. You may not know
the output, but you know the **invariant**. Reach for these:

- A quaternion round-tripped through a rotation matrix comes back unchanged.
- Every direction on the sphere lies inside at least one cell's **field of view** — the plan is
  complete by construction. Not the acceptance cone: that is how close you must aim, not what
  the frame captures.
- Registering a frame against itself yields identity rotation, all points inliers.
- Blending a single frame yields that frame.
- Exposure compensation across equally-exposed frames is a no-op.
- Selection is deterministic for the same candidates and policy.
- **An incremental rebuild equals a full rebuild, bit for bit.** Invalidate any node set, rebuild,
  compare against building from scratch. This single test is the safety net under the whole retake
  feature — write it before the incremental path exists.

Where only accuracy matters, the test-first artefact is the **harness**: a synthetic dataset with
ground truth and an asserted error bound. Write the bound generously first, tighten it later. A
generous bound that exists beats a precise one that doesn't.

Synthetic datasets come from `tools/` — render the frames a phone *would* have captured from a
known panorama, with known per-frame rotation, optional movers for ghost tests. Use them rather
than hand-collecting fixtures; they are reproducible and they carry truth.

### Where TDD does not apply

It is the default, not a ritual:

- **Spikes.** Exploring whether an approach is viable is fine — throw the spike away and write the
  real thing test-first. Say so in the commit message if a spike survived.
- **Browser plumbing.** A test asserting `getUserMedia` works is testing the browser. Test the
  adaptation — format conversion, error mapping, capability reporting — and let end-to-end cover
  the rest.
- **Visual output.** Nobody asserts whether a blend looks right. Use a perceptual-difference check
  against a stored render so *changes* are caught, and review the image by eye.

## Documentation

**Same-commit rule:** if a change makes a statement in `docs/` false, fix it in that commit. Not a
follow-up. If one change forces you to edit five documents, the docs have the coupling problem the
architecture is designed to avoid — say so.

**Write an ADR** (`docs/adr/`, four sections: context, decision, consequences, rejected
alternative) when you add or move a component, change a contract, add a dependency, take a layer-
rule exception, or reverse an earlier ADR. Supersede old ADRs; never edit one into agreement with
the present — the record of what we thought at the time is the point. The rejected alternative is
the section that pays off later.

**Don't** document: what the code already says, a duplicate of a contract header, how to run tests
in a fifth place, or status updates. A document with no job should be deleted.

## Repo structure

```
contracts/cpp/        the include root — headers consumed directly, never mirrored
core/                 C++: managers, engines, resource-access implementations, native adapters
  src/{managers,engines,resource_access,utilities}/
  test/               GoogleTest, mirroring src/; fakes in test/support/
bridge/               the WASM boundary: a C ABI over the shared heap, and the only tree
                      permitted to reference Emscripten. A client at its root; its
                      resource_access/ subtree holds browser-backed ports and is judged as
                      resource access. runtime.* is the composition root, exempt from the
                      layer rules by name (ADR 0014)
bench/                native CLI client — runs the core on datasets, prints timings
shell/                TypeScript PWA
  src/clients/{capture,review}/
  src/access/         browser resource-access adapters
  src/bridge/         generated facade + worker plumbing
tools/                Python: codegen, dataset generation, scoring, layer check
contracts/            IDL + interface headers (source of truth)
docs/                 principles, architecture, ADRs
```

An implementation lives in a directory named for the contract it implements —
`core/src/engines/pose_engine/null_pose_engine.cpp` — so that "component" means "the thing that
implements a contract". That is what makes the one legitimate same-layer dependency (a component
including its own contract) distinguishable from a component reaching sideways, and it is why two
implementations of the same interface share a directory: they are one component, not two.

Placement follows **layer, not feature**. Everything about coverage planning does not live in a
`coverage/` directory — the engine goes with engines, its test with tests, its contract with
contracts. Feature-shaped directories are how layer discipline erodes.

## Definition of done

- [ ] Test written first, failed for the intended reason.
- [ ] Implementation is the smallest thing that passes it.
- [ ] Affected docs updated in this commit.
- [ ] ADR written if the change qualifies.
- [ ] **`tools/gate.sh` is green.**

Run the whole gate, not the parts you think you touched. `tools/gate.sh` mirrors
`.github/workflows/ci.yml` step for step — layer and contract rules, the checkers' own test
suites, native, sanitizers, both WASM builds, size budgets, shell units, and the browser suite
against the built bundle (served with and without COOP/COEP, which is what the deployment target
does — ADR 0011).

A remembered subset is how a red build reaches the branch. The checkers' own suites
(`tools/test_*.py`) are the easiest to skip and the ones that have actually broken: they sit still
while you work, so nothing reminds you they exist. If a step is slow enough that you want to skip
it, that is a reason to make it faster, not to run less of it.

## Commits

One logical change, with its tests and doc updates. The message says *why* — the diff says what.
Keep pure reformats and renames in their own commit so review can skip them. Never commit a red
test to fix later; work in progress is not a commit.

## Working while the maintainer is away

When the maintainer says they will be away for a while, that is a standing instruction to carry on
alone under this loop rather than to stop and wait:

1. **Build the next thing**, test-first and to the definition of done above.
2. **Self-review it** before anyone else sees it — reread the diff adversarially, and sabotage
   every new test to prove it can fail. Check the sabotage *landed*: an edit whose pattern never
   matched leaves the code untouched and every test green, which is indistinguishable from a test
   that cannot fail. Assert the anchor before editing. Then check it landed in the second sense —
   name the test that should break and why, before running it. Text restored is not behaviour
   restored, and a sabotage that changes nothing accuses the wrong party.

   Self-review is also where a fix of your own gets the scrutiny a review would give it. The
   worst defect in a recent PR was not in the feature: it was a rollback added *during* this step
   to close a leak, which deleted the captured pixels it was unwinding. New code written to make
   old code safer is still new code.
3. **Open a PR and request a Copilot review.** Opening one does not summon a reviewer; ask
   explicitly, every time.
4. **Handle the findings.** Verify each against the code before acting: neither dismiss nor accept
   on sight. Fix it and push, or reply with the reasoning and the evidence.
5. **If the round contained a high-severity finding, request another round.** A round that found
   something real is a reason to look again; a clean round is where it stops.
6. **Never merge** until asked for that PR in particular. Green and mergeable is the finish line;
   merging is not yours to call.

Two habits make the difference between this working and it looking like it worked. **Measure
claims about the tooling** — when a finding asserts what CI, the browser or floating point does,
go and get the number (a job log, `elementFromPoint`, a one-line node script) rather than reasoning
from the diff. And **when the evidence clears the code only by accident**, say so and remove the
accident anyway.

## Recurring mistakes

| Symptom | What it usually means |
| ------- | --------------------- |
| Adding a fourth manager | A use-case variation of an existing one. Check the volatility map |
| An engine holding state between calls | It is a manager, or the state belongs in the caller. If the contract leaves the state nowhere else to live, the contract is the defect — name the state as a value the caller threads back in (ADR 0016) |
| A client importing an engine | The client is doing business logic, or a manager method is missing |
| A sabotage that leaves every test green | Before concluding the test is worthless, prove the edit applied. A `sed` or a string replace whose pattern did not match reads exactly like a surviving test, and it has happened more than once |
| A sabotage that applied, compiled, and changed nothing | Reinstating the *text* of the old code is not reinstating its *behaviour*. A rollback loop put back without the `push_back` that fills it iterates over nothing, and reads exactly like a test that cannot fail. Predict which test should break and why before running it; if a different one breaks, or none does, the sabotage is the suspect before the test is |
| A test that passes for a reason you did not intend | It proves whatever refused the input first. A version-gate test fed a document that was malformed anyway was refused by the parser, so the version line was never consulted. Make the input valid in every respect except the one under test — build it from a real artefact and corrupt one field |
| A guard no test can reach | Ask whether the state is reachable *in life* before deleting it. If it is, give the fake what it needs to get there — a lock it can refuse, a close that throws — and keep the guard with a test. If it is not, delete it. What you must not do is keep an untested guard because it feels safer |
| One edit that changed three call sites | A whole-file string replace hits every occurrence, not the one it was aimed at. Assert the match count, and prefer an anchor unique to the site |
| A test that passes alone and fails in the suite | Run what CI runs. `npx playwright test` covers `bridge/` and `shell/` in one worker; a `-g` filtered run is a debugging aid, never the check. The same goes for reading a `tail` of the gate instead of its whole output |
| A marker, overlay or reticle that is subtly in the wrong place | Ask which box its fractions are fractions *of*. The camera frame, the element it is drawn in and the pixels on screen are three different rectangles, and `object-fit` sits between the last two (ADR 0028) |
| Reaching for `cv::Stitcher` | See ADR 0005. It cannot do partial rebuilds, sensor priors, or mover-aware seams — the features that justify this project |
| A rollback that destroys what it was protecting | Before undoing work in an error path, ask what the thing being undone *is*. Forgetting an adopted frame frees the store — and drops the sink's only copy of a captured cell, so a restore that gave up halfway deleted the user's sphere to tidy up after itself. Leaving a bounded leak is almost always cheaper than deleting durable data, and an operation that is safe to repeat usually removes the need to undo anything |
| A pixel buffer in an interface | Use `FrameRef`; pin inside the core |
| `try` / `catch` near the boundary | Use `Result<T>` |
| Browser types in `core/` | A resource-access contract is missing or leaking |
| Holding a full-resolution burst for the whole sphere | ~15 GB. Only selected candidates are pinned; the rest rest encoded or spilled |
| "I'll add the test after" | The fakes already exist so you don't have to |
| Trusting the sensor pose as truth | It is a prior that seeds and bounds the search. Intrinsics and rotations are estimated |

## Reading order

`docs/00-principles.md` (why these rules) → `docs/03-architecture.md` (service map, call rules,
use cases) → `docs/02-volatility-map.md` (who owns which change) → `contracts/` (the interfaces
themselves) → `docs/04-runtime-topology.md` (threads, memory, the build graph).
