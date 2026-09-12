# 0056 — `accepted` says a minority backs the answer, and a refusal says there is no answer

**Status:** accepted

## Context

`PairwiseResult` carries both a `Result` status and an `accepted` flag, and until the first real
implementation of `EstimatePairwise` nothing had to say what the second one meant. The field had no
header comment at all — the one field in the struct whose meaning a caller cannot guess from its
name and type.

The first implementation set it from

```cpp
answer.accepted = bestInliers.size() >= kMinimumCorrespondences;
```

which is the same condition the function had already used, a few lines earlier, to decide whether to
refuse. So `accepted` was `true` on every result that was ever returned. A reviewer found it by
asking the only question that matters about a boolean: what makes it false? Nothing did.

Two candidate meanings were available and they are not the same:

1. **An answer exists.** Redundant with the status, as above.
2. **The answer is backed by enough of the evidence to be used as a constraint.** Distinct, but only
   if that can actually come apart from (1) on real data.

It does come apart, and the measurement is the reason this ADR exists rather than a comment. On a
twelve-frame checkerboard ring, three of eleven consecutive ORB pairs fail the gate — and the three
do not fail it alike, which is the point. **Two return a rotation** that fits its own support to
under a pixel, with 20 correspondences of 141 and 13 of 154 behind it. **The third returns nothing**:
no rotation gathered even the eight agreeing correspondences needed to answer, so it is a refusal.

Counting inliers under the *truth* rotation on those three pairs gives 11 of 128, 19 of 141 and 13
of 154. So on the two that answer, the search finds as many inliers as the correct rotation itself
has — the estimator is not failing, the support genuinely is a minority, because a checkerboard
hands ORB hundreds of corners that are indistinguishable and Lowe's ratio cannot separate what is
not separable.

## Decision

A refusal and an unaccepted answer mean different things, and both are documented on the field.

- **Refusal (`RegistrationFailed`)** — the two frames did not register, for either of two reasons:
  too few correspondences survived matching to fit a rotation at all, or enough survived and no
  rotation was agreed on by `kMinimumCorrespondences` of them. Both are the same fact to a caller —
  these frames do not go together — and the detail says which, for a human. Naming only the second,
  as an earlier version of this bullet did, leaves the commoner one undescribed.
- **`accepted == false` with an answer present** — a rotation was found and fewer than
  `kInlierFraction` of the correspondences agree with it. `relativeRotation` is the best the pixels
  offered and a caller may read it, but it is a weak constraint: a global solve should down-weight
  the edge or ask for another frame rather than chain it.

Concretely, `accepted` is `bestInliers.size() >= kMinimumCorrespondences && agreeing >=
kInlierFraction`, where the first conjunct is the refusal condition (so it is always true on a
returned result) and the second is the one that can be false.

`kInlierFraction` is set at 0.2 from the gap it has to straddle rather than from the observation: a
wrong correspondence lands within the three-pixel inlier radius by chance with probability about
1e-4, so agreement at any percentage is structure rather than luck, and the question is how much
structure to demand. It is deliberately not the middle of the measured spread (0.08 to 0.38 under
truth, detector depending), because a threshold set to an observation is one the next dataset moves.

The half-turn alias is *not* this gate's job. Those gather a real minority following and are
excluded by the prior's angular bound (ADR 0054's companion behaviour). Different failures,
different gates.

## Consequences

- The accuracy harness can no longer assert that every pair of a clean ring registers, because on
  this dataset ORB does not — and the honest bar is a usability one: a detector that cannot register
  more than half the consecutive pairs cannot drive a sphere. The published roadmap table gained a
  "pairs registered" column ahead of its accuracy column for the same reason.
- A detector that declined every pair would chain nothing and score a perfect zero, so the
  registered-pairs count is a conjunct of the accuracy test rather than a line in its output.
- `Refine` will have to decide what to do with an unaccepted edge. This ADR deliberately does not:
  nothing implements a global solve yet, and choosing a weighting before there is something to weight
  would be inventing a policy for an unwritten caller.
- **`kInlierFraction` is now two knobs wearing one name, and the second is expensive.** It is also
  the floor of `RansacSampleBudget`, because there is no point searching for a consensus smaller
  than the one the gate would refuse. So the budget is `log(1-p)/log(1-gate^3)` and lowering the
  gate costs cubically: at 0.2 it is 574 draws and a call takes about twenty milliseconds; at 0.01
  it is 4.6 million and the same call takes fifteen seconds. Measured, by lowering the constant
  during a sabotage and watching a twenty-millisecond test become a fifteen-second one. Nobody
  reading the constant's own line would expect that, and the target device is a phone — so both
  ends say so now. This is a cost this decision bought and it is not obviously the right trade: a
  separate search floor would decouple them, at the price of a number with no measurement behind it.
- **`PairwiseResult` had to grow a second field**, `correspondences`, which this ADR did not
  originally foresee. `accepted` is a comparison against a fraction, and the denominator was in no
  field of the struct — so the Rejected section's promise below, that the engine "reports the
  strength and lets the caller decide", was false as first implemented: one bit was reported and a
  caller holding `inliers = 11` could not tell eleven-of-forty from eleven-of-128.
- A caller reading `relativeRotation` without checking `accepted` gets a rotation that is usually
  right and sometimes minority-backed. That is a real hazard and the field's comment says so; the
  alternative — refusing outright — is in Rejected below.

## How its claims were measured

The inlier counts under the truth rotation (11 of 128, 19 of 141, 13 of 154) came from a temporary
`SPHANORAMA_TRUTH_Q` hook in `FitRotation` that counted inliers for a rotation handed in by the
test, printed them and was removed before the commit. It is named here because the numbers are
load-bearing for this decision and the harness as it stands **cannot reproduce them**: it scores
estimates against truth and never asks how much support the truth itself has. Anyone re-checking
this ADR needs to put that hook back. That the instrument is not in the tree is a weakness of this
record, and stating it is better than letting a reader assume a test covers it.

## Rejected alternative

**Refuse the minority-backed pairs outright**, folding `accepted` into the status and deleting the
field. Simpler, and wrong for two reasons. The rotations in question are *correct* — on the two
ORB pairs that answer, the search returns as many inliers as the truth rotation itself does, and the
answers are within a tenth of a degree — so throwing them away discards good information because the scene was
repetitive. And it would put the decision in the wrong place: whether a weak edge is worth using
depends on what else the graph has, which the engine cannot see and a global solve can. Reporting
the strength and letting the caller decide is the same shape as every other refusal-versus-report
choice in this core.
