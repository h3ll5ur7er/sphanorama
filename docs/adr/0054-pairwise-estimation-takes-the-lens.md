# 0054 — `EstimatePairwise` takes the lens, because a rotation cannot be recovered without one

**Status:** accepted

## Context

`IRegistrationEngine::EstimatePairwise` was declared, before anything implemented it, as

```cpp
Result<PairwiseResult> EstimatePairwise(const FeatureSet& a, const FeatureSet& b,
                                        const Quat& prior);
```

and `PairwiseResult::relativeRotation` is a `Quat`. Writing the first test against it — the
engineering skill's own invariant, that a frame registered against itself is the identity — surfaced
that those two facts cannot both hold with these parameters.

Two views of a distant scene that differ by a pure rotation are related by a homography

```
H = K R inverse(K)
```

where `K` is the calibration matrix built from the focal lengths and principal point. Matched
keypoints are pixels, so they determine `H` and nothing more. `R` falls out of `H` only once `K` is
known; with the lens unknown, an infinite family of `(K, R)` pairs explains the same matches, and
they disagree about the rotation by exactly the amount a wrong focal length implies. So the method
could not compute the thing its return type is declared to carry.

This was invisible for as long as the method refused. The stub returned `Unsupported` with a comment
saying matching was the next increment, and every signature looks implementable until someone tries.

`Refine` already takes an `Intrinsics`. Nothing about the pairwise step made it need one *less* — the
asymmetry was an oversight in the original declaration rather than a decision.

## Decision

`EstimatePairwise` takes the lens as a fourth parameter:

```cpp
Result<PairwiseResult> EstimatePairwise(const FeatureSet& a, const FeatureSet& b,
                                        const Quat& prior, const Intrinsics& lens);
```

**Passed, not held.** The layer rules say engines are stateless per session and that everything
except compute placement and pixel residency arrives as a function argument. An `Intrinsics` given to
the constructor would be per-session state in an engine, which ADR 0016 already ruled on for
`IPoseEngine` — and the failure mode would be worse here, because a stale lens produces a plausible
rotation rather than an error.

**`const`, and named `lens` rather than `initial`.** `Refine`'s `initial` is the value it goes on to
improve; the pairwise step reads the lens and does not refine it. The names say which.

The blast radius is small and entirely inside the core: `IRegistrationEngine` is an engine contract,
engines never cross the WASM boundary, and it carries neither `@boundary` nor `@facade`. So there is
no generated TypeScript mirror, no wire codec entry and no facade dispatch to regenerate — two
implementations and their tests, and nothing else.

## Consequences

- The caller must know the lens before it can register a pair. For the accuracy harness that is
  free: `truth.json` records the true intrinsics and the loader already returns them on
  `SyntheticDataset::lens`. For a real capture it is the camera's reported intrinsics, which is the
  same value `Refine` is given to improve, so the two steps agree about what they were told.
- **A wrong lens is now a way to be wrong quietly**, and it is worth naming because the refusal
  cannot catch it: feed a focal length that is 10% out and the estimate is a rotation, plausible and
  incorrect. The harness is the answer — the synthetic dataset's lens is true by construction, so
  accuracy measured there isolates the algorithm from the calibration. Accuracy on a real phone is a
  later and different measurement.
- `Intrinsics::rollingShutterLineTimeNs` is read by nothing here, and the pairwise step assumes every
  keypoint in a frame was seen at one instant. That is true of the synthetic dataset by construction
  and false of a phone. Named here rather than discovered later; it is not this increment's problem
  and it is somebody's.
- One more parameter on a method that already had three. The alternative was fewer parameters and no
  answer.

## Rejected alternative

***Answer in normalised coordinates and let `Refine` supply `K`.*** `EstimatePairwise` would return
the homography, or a rotation computed against a nominal unit camera, and the global step would
resolve the lens. Rejected on two grounds. It changes `PairwiseResult` from a rotation into something
that is not yet one, which makes the type lie in a subtler way than the signature did — and the
field is named `relativeRotation`. And it defers the only measurement Phase 2 exits on: a pairwise
rotation that cannot be compared against `truth.json`'s per-frame quaternions until a later stage has
run is not a number the harness can score, so the accuracy figure would wait on the global solver
rather than on the thing it is actually measuring.

***Hold the lens in the engine's constructor.*** Rejected above, under the layer rules. Worth
recording because it is the smaller diff and the more tempting one: it leaves three call sites
untouched and hides the dependency in a field.
