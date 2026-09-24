# 0062 — The rotation solver is a utility, and the eigensolver moves into the core

**Status:** accepted

## Context

`IRegistrationEngine::Refine` is declared and refuses. What it has to do is take the relative
rotations `EstimatePairwise` measures between pairs of frames, plus the per-frame sensor priors, and
produce one consistent set of absolute rotations — the step that turns a pile of pairwise
measurements into a reconstruction.

Two facts about that step decide where it lives, and they pull in opposite directions from the rest
of the engine.

**It needs no pixels.** `EstimatePairwise` reads frames, extracts features, matches descriptors and
fits a rotation with RANSAC — all of it OpenCV, which is why `FeatureRegistrationEngine` compiles
only where OpenCV does and a browser build gets the null engine instead (ADR 0052). The solve that
follows touches none of that. It is quaternion arithmetic over a graph whose nodes are frame indices
and whose edges are rotations. Put it inside the engine and it inherits a dependency it does not
use, in the one build where that dependency is absent.

**And the maths it needs already exists in the repository, in a place shipped code cannot reach.**
The solve sets each frame to the weighted average of what its neighbours say it should be, and that
average — Markley's, the principal eigenvector of the weighted outer-product sum — is exactly what
`core/test/support/rotation_scoring` computes to find the gauge alignment between an estimate and
ground truth. Same objective, same 4x4 cyclic Jacobi eigensolver, different reason for wanting it.
That eigensolver is not a free choice either: ADR 0049 measured power iteration exhausting a
200-iteration budget on 45.6% of wholly unrelated inputs at sixty frames, which is the size a real
sphere plans, and chose Jacobi because its cost does not depend on the eigenvalue gap at all.

So the alternative to moving it is a second copy of a measured decision — and a second place for
that measurement to go stale.

**This is not a new axis of volatility.** `docs/02-volatility-map.md` V7 is "how frames are
aligned", and its list already names *global refinement* alongside the detectors and the RANSAC
model. `RegistrationEngine` still owns that axis: it decides whether and how frames are aligned, and
it is the thing a caller talks to. What this ADR places is arithmetic underneath it.

## Decision

**Two components in `core/src/utilities/`, and the eigensolver crosses from test support into the
core.**

1. **`utilities/quaternion_average`** — `AverageQuaternions(rotations, weights)`. Markley's weighted
   average, carrying the Jacobi eigensolver ADR 0049 measured. Weights and per-input normalisation
   are new; the rest is the code that was in `rotation_scoring.cpp`.

2. **`utilities/rotation_averaging`** — `AverageRotations(edges, anchors, anchorWeight)`. The solver:
   relative rotations and per-frame priors in, one consistent set of absolute rotations out.

3. **`core/test/support/rotation_scoring` calls the first** rather than carrying its own copy.
   Test-side code depending on a core utility is the direction the layer rules allow; the reverse
   would not be.

**Why a utility rather than the engine.** Three reasons, in the order they matter. A browser build
can have it — the null engine is what a build without OpenCV gets today, and the day a composition
root wants a solve without a feature matcher, the arithmetic is there rather than behind a
`#ifdef`. It can be tested without a frame store, a lens or an image. And it keeps the one measured
eigensolver in one place, which is the whole of point 3.

**Why not a new volatility axis, and therefore not a new owner.** V7 already names global
refinement. A row here would claim `RegistrationEngine` no longer owns how frames are aligned, which
is false: the engine will build the edges, choose the weights, decide what an unaccepted pair is
worth, and answer for the result. The relationship is the one `camera_model` and `quaternion` have
to the engines that call them — arithmetic a component uses, not a component that decides something.

**With the caveat `docs/02-volatility-map.md` insists on**, and which this paragraph should not be
read without: those two are called from `core/src` today and `rotation_averaging` is not. The
analogy is to the *shape* of the relationship, not to its current state, and the Consequences below
say what the difference costs. A reviewer found the two documents disagreeing about whether the
caveat was needed; it is, and it belongs in both.

**What this does not decide.** `Refine` still refuses, and wiring it up is a separate change with a
separate question in front of it: `GlobalSolution::intrinsics` is documented as "shared across
frames, refined here", and `PairwiseResult` carries a *count* of correspondences but not the matched
points, so there is nothing in the engine's input to refine a lens **from**. That is a contract
question and gets its own ADR rather than a pass-through that quietly makes a documented promise
false.

## Consequences

**One component in `core/src` that nothing in `core/src` calls.** `rotation_averaging` is it:
`quaternion_average` has a caller from the moment it exists, since the solver includes it and calls
`AverageQuaternions` in its sweep. The first version of this section said "two", which a reviewer
caught — and the count is load-bearing rather than cosmetic, because it is both the exception this
ADR takes and the condition under which it should be superseded.

This is the cost worth stating plainly, because the volatility map's own line elsewhere is that a
thing nothing in `core/src` reads does not earn a component. The exception is narrow and
time-limited: the caller is `IRegistrationEngine::Refine`, it is named above, and the reason it is
not written yet is a contract gap rather than a change of mind. Until then `rotation_averaging` is
compiled into the core — including the WASM build, where it costs size budget — and reached only
from tests. If the `Refine` wiring does not follow, this ADR is the thing to supersede.

**The WASM builds carry no such code, and this section said they did.** `sphanorama_core` is a
static archive and nothing references `AverageRotations`, so the member is never extracted. The
exception costs **zero bytes** until a caller exists.

Measured by removing both sources from `core/CMakeLists.txt` and rebuilding: `sphanorama-core.wasm`
is byte-identical, 266,819 bytes and the same MD5, with and without them. That experiment rather
than a symbol dump, because the symbol dump does not work here and saying so is the useful part —
the release wasm has no usable name section (`ArmBurst` and `Normalize` are certainly linked and
appear nowhere in it), and the archive members are LLVM bitcode that the system `llvm-nm` cannot
read at all. A first pass "confirmed" this consequence from a `0` that was a tool failure.

That is better for this ADR than what it claimed, and it moves the real cost somewhere the original
sentence pointed away from: compiled-but-unlinked is the one regime a size budget cannot police, so
the gate's size step will not notice the day a composition root wires `Refine` up and the code
arrives in the binary for the first time. The budget is a backstop for that change, not evidence
about this one.

**ADR 0049's banner now points at a file that no longer holds its figures.** The banner says
"`core/test/support/rotation_scoring.cpp` carries the current figures where the code is", and the
code moved. 0049 is not edited into agreement — that is the rule the banner itself exists to serve
— so the pointer is added here instead: **the Jacobi sweep figures live in
`core/src/utilities/quaternion_average.cpp`.** ADR 0049's decision is untouched by this move; only
the address of its evidence changed.

**A second copy of a published figure was created and then removed.** `rotation_scoring_test.cpp`
still carried "five working sweeps" from before 0049's own re-measurement; it is corrected here
rather than left in a component that no longer owns the code.

**The sweep budget is a new measured number and it will be quoted.** `kMaxSweeps` is 1000, chosen
from a table in `rotation_averaging.cpp`. That table has one *prose* copy, the paragraph beneath it,
and one *executable* copy — `EXPECT_EQ(believed.sweeps, 590)` in `rotation_averaging_test.cpp`, which
is the `[12 frames, 0.01]` cell. This section said "the only copy" and was wrong about the one that
matters most, since it is the one that would catch a change rather than merely disagree with it.

**And then it was wrong about what that assertion catches**, which is worth recording because the
error was the same shape twice: a claim about coverage made from the case in front of it. It read
"the only assertion anywhere that fails when `kSettledDeg` or `kMaxSweeps` moves". Measured, it is
neither. `kSettledDeg` at 1.786e-6 fails it *and* two other tests. `kMaxSweeps` at 700 does not fail
it at all — 590 is under any budget worth setting — and the only pin on that constant anywhere is
`EXPECT_EQ(solved.sweeps, 1000)` in `ASolveThatRunsOutOfSweepsSaysSoAndStillAnswers`. So the two
constants have two different guards, and this ADR named one of them for both. Anything quoting
the table inherits the obligation the accuracy table has: the figures move together or not at all.

**A third native preset, and a CI job for it, exist because of this component's review.** Rounds 9,
10 and 11 each found a caller that gated a quaternion with one evaluation of its norm and divided by
another, and the two disagreed near the ends of the admissible range — an admitted quaternion came
back as the identity or as the zero quaternion, and a frame landed up to 169 degrees out with
`valid` true. None of it could show on a build CI ran, because at `-O0` every evaluation of an
expression is compiled the same way. `native-contracting` is clang at `-O3 -ffp-contract=fast` on
`x86-64-v3` (a fixed target, because the runner's AVX10 CPU made `-march=native` a `-Werror`
failure), without OpenCV, and its job refuses to pass unless `-ffp-contract=fast` is the last word
on every compile in the build's compile database (`tools/fused_build_check.py`).

The cause was vectorisation, not inlining, which the first version of this paragraph said. Read
from the object code: the gate's copy of the sum of squares compiled to a scalar fused chain and
`Normalize`'s to a vectorised multiply and three plain adds, in the same file. So the fix is in
`Norm` rather than at the call sites: it fuses by hand with `std::fma`, whose rounding the language
fixes, and every compiled copy now agrees. `UsableNorm` stays because it spares a caller the
second evaluation, and `Quaternion.EveryQuaternionTheGateAdmitsNormalizesToItsOwnRotation` is the
test the job exists to run — on the fused build it failed seven times before the fix.

**The fix has a price where there is no hardware fused multiply-add.** Measured by the round-12
arithmetic reviewer: `Normalize` went from 8.5 to 91 ns in WebAssembly, where `std::fma` is a
software routine, and from 5.9 to 11.9 ns on baseline x86-64; the core's gzipped wasm grew by 696
bytes. At today's call counts that is microseconds per tick. It becomes roughly a tenfold cost on
the solver's inner loop once that runs in the browser, which is where to look first if it shows.
And a norm spelled out by hand anywhere else now disagrees with the gate on every build rather than
only on fused ones — `RotationMatrix` in the registration engine did, and was moved onto
`Normalize` — so a second copy of the sum of squares is a defect wherever it appears.

**Normalising each input changed behaviour, in a case nothing was reaching.** `IsUsableRotation`
admits any finite norm above 1e-12, and an unnormalised quaternion contributes its *squared* norm to
the outer-product sum — a second, unasked-for weight, so a tripled quaternion counted nine times.
`rotation_scoring`'s inputs were always unit, so its answers do not change; the new caller holds
rotations recovered from fitted matrices, which are unit only to the precision of the fit.

## Rejected alternatives

**Put the solve inside `FeatureRegistrationEngine` and leave the eigensolver where it was.** The
smallest diff, and it fails on the browser build: `Refine` would then exist only where OpenCV does,
for arithmetic that never calls OpenCV. It also leaves two copies of the eigensolver, which is the
duplication this repository has already been bitten by — and the copy in test support is the one
carrying a measurement, so the drift would be silent and would be in the evidence rather than in the
code.

**Leave the eigensolver in `core/test/support` and have the core utility call it.** Not permitted
and not desirable: shipped code cannot depend on test support, and a build that excluded the tests
would not link.

**Make it a fourth engine — a `GlobalSolveEngine` behind its own contract.** This is the option that
needed the most thought, because a solver is an activity and activities are engines here. It loses
on the volatility question: V7 already names global refinement, so a new contract would split one
axis across two components and a change to how frames are aligned would touch both. The engineering
skill's rule is that a component already owning the axis gets extended rather than joined. And there
is nothing for the contract to abstract yet — one implementation, no second strategy in view, and
`IRegistrationEngine` is already the interface a caller talks to.

**Write `Refine` now and return `initial` as the refined lens.** Rejected for the reason the
Decision names: `GlobalSolution::intrinsics` says "refined here", and an implementation that returns
its input unchanged makes that sentence false while looking like it works. A refusal is legible and
a pass-through is not. The contract question gets its own ADR, and the code waits for it — which is
ADR 0054's precedent, where a signature that could not honour its own return type was treated as a
gap to fix rather than a stub to hide it in.

**Defer all of it until `Refine`'s contract question is settled.** Tempting, and it would avoid the
"nothing calls it" cost above. Rejected because the two questions are independent: how a rotation
solve is computed does not depend on how a lens is refined, and the solve is the part with a number
attached to it. Deferring would have meant the contract discussion happening with no measurement of
what the closing edge is worth — and the measurement is what says the discussion is worth having.
