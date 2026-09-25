# 0065 — `Refine` solves rotations, ties each prior to its frame, and passes the lens through

**Status:** accepted

## Context

`IRegistrationEngine::Refine` has refused since it was declared. ADR 0062 put the maths under it in
`utilities/rotation_averaging` and named what stopped it being wired up; ADR 0064 made that solver
able to take the anchor weight a registered ring asks for. What is left is in the contract:

- **The priors are not tied to frames.** `Refine` takes `std::span<const PoseSample>`, and a
  `PoseSample` carries a timestamp, an orientation and rates but no `FrameId`. The pairs name their
  frames; nothing says which prior belongs to which, and matching by timestamp would be a guess.
- **Nothing can refine the lens.** `GlobalSolution::intrinsics` says "refined here", but a
  `PairwiseResult` carries a *count* of correspondences, not the correspondences. A focal length
  cannot be fitted to counts.
- **`GlobalSolution::medianResidualPx` cannot be computed** for the same reason — a pixel residual
  needs pixels — and a zero there reads as a perfect fit.

The capture already records what the first gap needs: every `Candidate` holds the `PoseSample` it was
taken at beside the `FrameRef` of its pixels.

## Decision

**`Refine` solves for rotations now, and the lens is a later step with its own ADR.**

1. **Priors are tied to frames**, by a contract type that carries each frame's prior with its
   identity:

   ```cpp
   struct FramePrior {
     FrameId frame;
     PoseSample pose;
   };
   ```

   `Refine` takes `std::span<const FramePrior>`. The priors are the frame set, and their order is the
   order of `GlobalSolution::frames`.

2. **Each accepted pair is an edge, weighed by its inlier count**, in the convention
   `rotation_averaging.h` states for `EstimatePairwise(a, b)`. An unaccepted pair is left out rather
   than weighed at zero — ADR 0056 made `accepted` exactly the question "should a global solve use
   this edge" — and left out so that whatever it carries cannot refuse the solve. That narrows ADR
   0056, which expected a global solve to use one as a weak constraint.

3. **The anchor weight is the engine's constant, and it is measured**: 0.01 against inlier counts.
   On the photograph ring (ADR 0064's table) inlier-weighted edges give the same shape from 1e-6 to
   0.1 within 0.003 degrees for every detector, and start to pay at 1. 0.01 sits four decades inside
   that plateau from below and one from above. Inlier weighting is chosen over one per edge because
   it moves the plateau's upper edge up by about two decades, from near 1e-3 to near 0.1 — the
   inlier counts are about a hundred — which is what gives 0.01 room on both sides: per edge, 0.01
   already costs SIFT a factor of 1.7. Not a `Refine` parameter: no caller has a policy to pass, and a knob only a test turns is
   a second copy of the constant.

4. **The lens is passed through, and the contract says so.** `GlobalSolution::intrinsics` is
   `initial`: the lens the rotations are expressed under, which a compositor needs beside them. Its
   comment changes from "refined here" to say exactly that, and that refinement is a later step.

5. **The fit is reported in the unit the solve has.** `medianResidualPx` is replaced by
   `medianEdgeErrorDeg`, `maxEdgeErrorDeg` and `edgesUsed` — how far the answer leaves the pairs it
   used, and over how many — `pieces`, how many separately placed pieces the pairs left, and
   `priorsUsed`, how many priors had a say in which way it faces,
   without which a reconstruction one surviving prior pinned reads better than one twelve agreed on. `droppedFrames` becomes the list of frames the solve could not place, which
   are left out of `frames` and `rotations` so that neither ever holds a value that is not a solved
   rotation. `priorOnlyFrames`, `ambiguousFrames` and `converged` carry the solver's other honesty
   fields across, because each says something the error figures cannot.

6. **A prior counts only if its `confidence` is above zero, and zero is the only way to say there
   is none.** Zero confidence is a direction relative to wherever the sensor
   started (ADR 0041); averaged with anchored priors it turns the whole answer toward that accident,
   45 degrees in a reviewer's probe. `ArmBurst` refuses to fire on one, so a burst-captured frame
   always has confidence; a frame given through `OfferFrame` — import, replay, manual shutter —
   carries whatever pose its caller supplied, and one with no confidence is placed through its
   pairs. A default `PoseSample` — confidence zero — is then no prior rather than a claim the phone
   was held level. Any other way of looking absent is refused (decision 7): a confidence outside
   [0, 1], or an orientation that is not a rotation where the confidence claims one, is a defect
   upstream. Read as no prior, a NaN from a caller's arithmetic left `priorsUsed` one short with no
   frame named, and on every prior at once was sent to the sensor as `FailedPrecondition`, in a
   reviewer's probe. A first version of this decision read an unusable orientation as no prior.

7. **Refusals**, each with the reason in the detail: `InvalidArgument` for no priors, an invalid or
   repeated `FrameId` among them, a prior whose confidence is outside [0, 1] or whose orientation is
   not a rotation while its confidence claims one, a lens `IsUsableLens` refuses (it is not read by the solve, but
   it is returned as the lens the answer is expressed under), a pair naming a frame with no prior, a
   pair from a frame to itself, or an accepted pair whose rotation is not one or whose counts no
   engine fills in — no inliers, or fewer correspondences than inliers. A rotation never written is
   one of those: `PairwiseResult::relativeRotation` defaults to `Quat{0, 0, 0, 0}` rather than
   `Quat`'s identity, as the solver's own edge type does, because a pair whose counts were filled
   in and whose rotation was not otherwise claims at full weight that its frames share an
   orientation — thirty degrees of shape on an open chain in a reviewer's probe, with every figure
   of the answer reading clean. `EstimatePairwise` never produces those, and an accepted pair with no inliers weighed at zero would be left out silently
   while its frames were named prior-only or dropped. A pair naming a stranger is refused even
   unaccepted: it is a caller and a capture disagreeing about which frames exist, not a measurement
   to disbelieve. `FailedPrecondition` when every prior's confidence is zero — `ArmBurst`'s code for the same
   condition, since the remedy is the sensor rather than the pixels — because then nothing fixes which way
   is up, decided after every prior and pair is checked so that a malformed one is always the
   caller's defect; `Internal` for a solver refusal these checks did not anticipate, rather than a guess at
   which of the caller's it was. A frame with no prior is not a refusal: it is placed through its
   pairs, or dropped.

8. **In `FeatureRegistrationEngine` only.** The null engine keeps refusing: its `EstimatePairwise`
   refuses everything, so a `Refine` there would have nothing to solve.

## Consequences

- A contract change in `types.h` and `engines/registration_engine.h`, and a regenerated
  `contracts/ts/contracts.d.ts`. Nothing reads `GlobalSolution` yet, so nothing else moves.
  `PairwiseResult::relativeRotation`'s braced default is the first in a contract header, so
  `tools/contract_gen.py` now splits declarators only at commas outside braces.
- **`rotation_averaging` has its caller in `core/src`**, which is the condition ADR 0062 named for
  its exception ending. Only in the OpenCV build, so the WASM builds still carry none of it.
- **The measurement the roadmap was waiting for exists.** Solved through `Refine` with all twelve
  pairs and each prior three degrees out about an axis of its own, the photograph ring's median error
  is 0.0552 / 0.0348 / 0.0264 degrees (ORB / AKAZE / SIFT), worst frame 0.1164 / 0.0624 / 0.0674,
  against the chain's medians 0.1009 / 0.0612 / 0.0239. The closing pair roughly halves ORB's and
  AKAZE's error and leaves SIFT's a little worse. Not through the priors — the shape is flat in their
  weight from 1e-6 to 0.1 — and why is not yet known. The solve faces within 0.0002 degrees of where
  the priors agree, which is 0.26 degrees from the truth. Bounded in `registration_accuracy_test.cpp`
  at a median of 0.08, which is what fails a solve handed eleven pairs instead of twelve (ORB then
  reads 0.1005).
- **A capture whose priors are all unanchored is refused**, with `FailedPrecondition`. A gyroscope
  alone reports zero confidence for its whole life, and its priors agree with each other well
  enough to solve with — but `ArmBurst` already refuses to capture on them. A capture built through
  `OfferFrame` with unset poses does reach here, and is refused; one that mixes offered and burst
  frames loses the offered frames' priors, which are placed through their pairs. If an import path
  ever needs its own priors counted, this is the line to revisit.
- **`pieces` says how much of the answer the pairs actually placed.** A ring two declined pairs cut
  in two is two pieces whose seams sit where the priors put them — 1.84 degrees out in a
  reviewer's probe, with every other field reading clean.
- **The lens is whatever the caller had.** On a real phone that is the page's reported field of view
  until refinement exists.
- **Step 2 is lens refinement,** and needs the inlier correspondences carried out of
  `EstimatePairwise` — a contract change with a memory cost, hundreds of points per pair. A lens
  persisted per device is the natural `initial` for it, and `Refine` already takes one; where that
  lens lives is a new volatility axis (what this device's camera is known to be) with no owner in
  the volatility map, and it is decided with step 2.

## Rejected alternatives

**Add a `FrameId` to `PoseSample`.** A pose sample is produced by the motion port long before any
frame exists, and almost none ever belong to one; the field would be meaningless on nearly every
value of the type.

**Pass a `std::span<const FrameId>` parallel to the priors.** Two spans that must stay the same length
and in step are the second copy this codebase keeps paying for. One span of pairs cannot drift.

**Weigh unaccepted pairs at zero.** The solver refuses the whole input over one edge whose rotation
is not a rotation, whatever its weight, so an edge the caller has already disbelieved could refuse a
solve it contributes nothing to.

**Drop `intrinsics` from `GlobalSolution` until it can be refined.** Honest, and it would make every
compositor signature take a lens beside the solution — a second copy of the one fact the rotations
cannot be read without. Kept, and described as what it is.

**Read every prior whatever its confidence**, which this ADR's first version decided, so that a
gyroscope-only phone's captures could be solved. It would solve them, and it would average a prior
nobody anchored in with the ones somebody did: one such prior forty-five degrees out turned a
reviewer's whole reconstruction toward it. The phone it was for cannot capture by burst at all, and
a capture offered frame by frame with unset poses is the case the refusal above names.
