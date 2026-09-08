# 0046 — The lens is a utility, and a pixel it cannot invert is refused

## Context

`Intrinsics` has been in `contracts/cpp/sphanorama/types.h` since the architecture was written. It
is passed to `ICoveragePlannerEngine::Plan` and to `IRegistrationEngine::Refine`, stored on
`CaptureSessionManager`, and written into every persisted session. Until this change **no code in
the repository read it as optics.** Two fields are read — `capture_session_manager.cpp` writes
`lens.width` and `lens.height` into the session document — and they are read as a frame size, which
is what those two are. Nothing has ever read the focal length, the optical centre or a distortion
coefficient. Both `Plan` implementations take the parameter unnamed and tessellate from
`CapturePlanSpec`'s field of view instead; `Refine` is null; and `CaptureSessionManager` leaves all
seven optical fields at zero, which is correct — the struct's own comment says a lens is *estimated
during a build*, and there has been no build.

Phase 2 is the phase that estimates it, and every stage of Phase 2 is this transform or its
inverse: rendering a synthetic frame from a known panorama, measuring a match residual in pixels,
seeding a rotation from a sensor prior, refining focal length and distortion in a bundle
adjustment, resampling into equirectangular output. So the projection comes before any of them and
all of them consume it.

Three things about it are easy to get wrong and invisible when wrong:

- **The Y axis flips.** Camera space is +Y up (ADR 0015, ADR 0017, and `Direction` in the utilities
  bar); a raster image is +y down. A model that is consistently upside down round-trips perfectly
  and stitches a sphere that is upside down.
- **Brown-Conrady has no closed-form inverse.** It is solved by iteration, and an iteration that
  has stopped moving has not necessarily stopped in the right place.
- **A distortion strong enough stops being invertible at all.** Where `r · radial(r²)` stops
  increasing the image folds back over itself, two directions land on one pixel, and there is no
  answer to give.

## Decision

**The lens is a utility — `core/src/utilities/camera_model.{h,cpp}` — not an engine**, and
**every way of having no answer is a refusal rather than a pixel.**

- Camera space is **−Z forward, +Y up, +X right**, the single frame §3.5 of the architecture
  already establishes. Image space is **+x right, +y down**, origin at the image's top-left corner.
- Distortion is **Brown-Conrady in OpenCV's parameter convention**, so `k1 k2 p1 p2 k3` measured by
  any other tool drops into `Intrinsics` unchanged.
- `Project` and `Unproject` answer a small struct carrying a `valid` flag. They refuse an unusable
  lens, a direction that is not a measurement, a direction at or behind the plane through the
  optical centre, and a radius the distortion does not invert up to.
- **"Up to", not "at".** The fold test is a statement about the whole ray from the optical centre
  outward, because a local one is worthless: the radial slope `1 + 3k₁r² + 5k₂r⁴ + 7k₃r⁶` is a cubic
  in r² and can dip negative and come back, so a radius on the far side of the dip looks healthy
  where it lands and is folded over all the same. The radial half is settled in closed form — the
  slope is 1 at the centre, so it is enough to test the endpoint and the slope's own turning points
  inside the interval. The tangential half cannot be: `p1` and `p2` break the reduction to one
  dimension, so the Jacobian determinant of the full map is **sampled** along the ray, and a fold
  thinner than a sixty-fourth of it would be missed.
- **The field of view is measured through the model**, by asking where the two opposite edges of the
  frame actually look, rather than computed from the focal length. Distortion moves it: on a
  moderately barrelled lens `fx` alone says 66° for a frame subtending nearly 73°. An edge with no
  preimage answers 0 — the contract's silence — because the lens has no left-hand side to measure
  to.
- `Unproject` iterates, then **checks its answer by calling `Project` on it** and refuses if the
  result does not land back on the pixel it was given. Not by repeating `Project`'s arithmetic
  locally: the fold refusal is a property of `Project`, and asking it is the only way the two are
  guaranteed to agree about where the fold is.
- `LensFromFieldOfView` is the only place intrinsics are *invented*. It exists to seed an estimate
  and to render synthetic datasets whose ground truth is known because the lens was chosen, and it
  does not set `estimated`.

## Consequences

- Phase 2 has its shared arithmetic, and one definition of it **inside the core**: the registration
  engine, the coverage planner and the composition engine all measure angles the same way or they
  fail a test. The synthetic-dataset renderer is deliberately *not* in that list — see the last
  rejected alternative, which is the whole reason it renders through its own implementation. An
  earlier draft of this bullet had it sharing the definition, which is the opposite of the decision
  taken forty lines further down.
- A default `Intrinsics` — the one every capture session is holding right now — answers `false` to
  `IsUsableLens`, and a test says so by name. Nothing can begin quietly trusting a zero focal
  length without that test going red first.
- **Newton arrives on the full step, not the damped one.** The damping halves a step until it is
  defined and strictly closer; when Newton has converged exactly the step is *zero*, every halving of
  zero lands on the point the iterate already occupies, and "strictly closer" rejects all thirty in
  turn. The settled-step exit that exists for this was testing the accepted step and sitting after the
  loop, so it could never run. Measured: about thirty wasted evaluations on every converged solve, and
  **8.7×** the cost on an undistorted lens (0.668 → 0.077 µs/px). Testing the full step before the
  damping fixes it, with the answers unchanged over 743,175 accepted pixels.

- **The inverse is solved by Newton's method, and getting there took three tries.** The obvious
  choice is the fixed point `xn ← (xd − tangential) / radial`, which is what shipped first. It is
  wrong for the problem. Its multiplier at the solution is `|2u·R′(u)/R(u)|`, and that equals exactly
  1 *at* the fold — for any radial polynomial, since the fold is defined by `R + 2u·R′ = 0`. So the
  two conditions share a boundary by algebra rather than by luck, and an earlier draft of this bullet
  credited `k₁` with a coincidence that was never `k₁`'s to supply; a reviewer caught it.

  What a **pure `k₁`** lens supplies is that the slope has a single root, so "multiplier below 1" and
  "the map inverts" are the same *interval* and not merely the same boundary. That is why the defect
  survived a round of review conducted against radial-only lenses. Add `k₂` and the slope can dip and
  return, the two regions come apart, and the fixed point diverges *inside* the certified one: a 115°
  lens with `k₁ = −0.3, k₂ = 0.1` folds **nowhere** at any radius, every pixel has exactly one
  preimage, and it sat in a 2-cycle 0.7 normalised units from the answer, refusing a quarter of the
  frame at 500× the cost of finding it.

  Two rounds were spent raising the budget — 500, then 5000 — against what looked like slow
  convergence near a fold and was actually divergence away from one. Both raises made a wrong answer
  more expensive without changing it, and both were written up here as though the number were the
  finding. Newton uses the Jacobian this ADR already required for the fold test, converges
  quadratically, lands in four to six passes, and reaches solutions the fixed point cannot approach.
  The ceiling is 20, and a refusal after 20 is a genuine failure rather than a budget expiring.

  **And then damped, a round later.** Newton alone traded one family of refusals for another. Its
  first guess is the distorted coordinate: on a *barrel* lens (k₁ < 0) that sits inside the answer and
  the iteration walks outward, which is why every lens in the tests was fine — all of them had
  k₁ ≤ 0. On a *pincushion* lens the guess sits outside, so a full step overshoots the fold, or the
  guess is already past it, and the `radial > 0` and `determinant > 0` guards refuse — **because they
  judge the iterate rather than the pixel.** A reviewer found a direction the model certifies, a pixel
  this file's own `Project` produced from it, and an inverse that would not take it back; on a
  stronger pincushion, 168 of 4,941 in-frame pixels.

  Two changes close it. The guess is pulled toward the optical centre until the model is defined
  where it stands — the centre always qualifies, so it terminates. And each step is halved until the
  trial point is both defined and *closer than where the iterate stands*. Requiring the decrease
  rather than mere definedness is what stops it cycling, and it is worth its own note: measured by
  building the file both ways and diffing, 15 pixels of 462,969 differ and **every one is accepted
  only with the condition**. It widens what can be answered.

  Measured after — and **narrowed rather than closed**, which the first draft of this bullet claimed
  and a reviewer disproved. Over six lens families, barrel and pincushion, there are zero refusals of
  anything `Project` accepts. Over a wider grid of 625,953 in-frame accepted pixels there are **six**,
  all at extreme tangential distortion (`p1 = p2 = 0.4`) combined with a strong negative `k₂` — a
  0.0010% residue. Worst round-trip error 1.2e-06 degrees, median 1 to 5 passes, longest tail 12.

  "Zero refusals over six lens families" was true and was not the claim a reader would take from it.
  The invariant `Project` accepts ⇒ `Unproject` answers is one this model *aims* at and does not
  fully reach, and saying so is worth more than a number chosen from where it happens to hold.

  **What the earlier numbers were measuring.** On a `k₁ = −0.9` lens the fixed point accepted pixels
  out to 89.6% of the invertible radius at a budget of 20 and 99.6% at 100. Those figures were real
  and they were beside the point: they describe how far a bad solver gets, not what the model can
  answer.

- **There are two failures here and they have different catchers**, which the first draft of this
  ADR ran together and a reviewer separated:
  - A **settled** fixed point is, by construction, a solution of the forward equation — so it is
    always a genuine preimage. "Converged on something that is not a preimage" is not a state this
    iteration can be in; a 223,608-sample sweep found zero of them. What the round-trip tolerance
    actually catches is **exhaustion**: an iterate that had not arrived when the budget ran out.
  - Landing on a genuine preimage that is the **wrong one of two** is the other failure, and the
    tolerance is structurally blind to it — the answer projects back exactly, because it really is a
    preimage. Only the fold test catches that, which is why the fold test has to be right.
- **Two of these consequences were written before the review and were wrong.** The fold test was
  local where it promised to be global, and the field of view read the focal length while claiming
  to say how much of the sphere a frame covers. Reviewers produced, independently, a lens answering
  an in-frame pixel with a bearing **86.8° wrong** — a genuine preimage, just the wrong one of two,
  which is the one class of error a round-trip check is structurally blind to. Both are fixed above,
  and the record is left here rather than tidied because the pattern is the point: every one of these
  came from a *local* test standing in for a *global* promise.
- **Most guards in the file have no test of their own** — measured over all twenty-nine, one at a
  time: thirteen are caught and sixteen are not — and that is the price of a policy rather than an
  oversight. (The count has been wrong twice: once from a hand-picked subset, once from being right
  about code the solver then replaced. It is re-measured with every change to the solver now.) Each makes the next step's precondition locally true, so correctness never rests on
  NaN propagating through a polynomial, a guarantee `-ffast-math` withdraws. The implementation says
  so at the top rather than letting each guard imply it is the sole refuser.

  Two of them turned out to be load-bearing, both found by sweeping rather than reading, and both
  now tested: `Unproject`'s `check.valid` (a refused pixel is `(0, 0)`, which compares equal to an
  input of `(0, 0)`) and `Project`'s `isfinite(u, v)` (with `k3 = 1e293` every intermediate is finite
  and the overflow happens at `fx * xd` alone — no NaN, no backstop). The second is a counterexample
  to the policy's own reasoning, which is why the policy is now stated as a policy and not as a
  proof. An earlier draft of this bullet said "thirteen of fourteen" from a subset measurement; the
  claim about measurement discipline had not itself been measured.
- Cost: two conventions now have to be kept in step by hand — this model's, and whatever the
  synthetic-dataset generator uses to render. That is deliberate (see below) and it is why the
  agreement will be a pinned test rather than a shared function.

## Rejected

***A `LensEngine` behind a contract.*** Engines own a volatility axis and are swappable
implementations of a stated activity. Projection is neither: it is arithmetic that the registration
engine, the coverage planner, the composition engine and the bench tool all need, and an engine
that four engines called would be engines calling engines, which the layer rules forbid outright.
The volatility that *is* real here — which distortion model, how many coefficients — lives inside
`Intrinsics`, and a richer model changes that struct rather than adding a component.

***A NaN pixel to mean "no image".*** It reads well and it is the sentinel trap: NaN is producible
by the ordinary arithmetic in this file — a zero depth alone will do it — so it cannot also carry
the meaning "there was no answer". A caller could not tell a refusal from an overflow, and the one
place that matters is the one place nobody looks.

***A closed-form approximate inverse*** — fitting a polynomial to the inverse distortion, as some
pipelines do. Faster, and it never refuses. That is the objection: its error is silent,
lens-dependent, and largest exactly at the frame edge where the overlap between two cells lives, so
it would spend its accuracy where registration needs it most and say nothing about having done so.

***Sharing one implementation with the synthetic-dataset generator.*** Tempting — two copies of a
projection will drift. But a harness that renders its frames with the same code the engine under
test uses cannot measure that code: a wrong camera model would render frames consistent with itself
and score perfectly. Independent implementations are the point (§5.4 of the toolchain document says
the same about the reference), and the drift is handled by pinning their agreement in a test rather
than by removing one of them.
