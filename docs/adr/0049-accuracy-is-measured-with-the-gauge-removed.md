# 0049 — Accuracy is measured with the gauge removed, and the median is the number

**Status:** accepted

## Context

Phase 2's exit criterion is "synthetic-dataset registration median error under a stated angular
threshold". That sentence has been in `docs/06-roadmap.md` since before there was anything to
measure, and it hides a decision nobody had made: **what "error" means when comparing a
reconstruction against ground truth.**

The naive answer — compare each estimated rotation against the true one and average — is wrong, and
wrong in a way that would have made the harness useless rather than merely imprecise.

A panorama is reconstructed from how frames sit *relative to each other*. Nothing in the pixels says
which way is north. So the world frame the reconstruction lands in is arbitrary: take a perfect
reconstruction, turn every frame by one common rotation, and you have described the identical
panorama in different coordinates. `Quat` is device -> world (`types.h`), so that freedom acts on the
left — `{q_i}` and `{G (x) q_i}` are the same answer for any rotation `G`.

Compared frame by frame, those two score very differently. A perfect reconstruction expressed in a
world frame 30 degrees from the dataset's would report 30 degrees of error **on every frame at
once** — which is exactly the signature of a catastrophically broken registration. An instrument
that cannot tell the best possible answer from the worst one is not a weak instrument; it is a
misleading one, and it would have been consulted on every detector comparison and every tuning
change in Phase 2.

This has to be settled before the registration engine, not after, for the reason the roadmap already
gives for the harness as a whole: a measuring instrument written after the thing it measures gets
tuned until it agrees.

## Decision

**Score by first removing the gauge, then measuring what is left, and report the median.**

- `core/test/support/rotation_scoring.{h,cpp}` finds the rotation `G` that best carries the estimated
  set onto the truth set, and reports the per-frame angle between `G (x) q_i` and `r_i`.
- "Best" is **Markley's chordal average**: the `G` maximising the sum of squared quaternion dot
  products, which is the principal eigenvector of the residuals' outer-product sum. Squaring is what
  makes the `q` / `-q` sign question disappear rather than needing handling — the outer product is
  invariant under negation, because negation is not a different rotation.
- The eigenvector is found by **cyclic Jacobi**, not power iteration. See the consequences: this was
  a measurement, not a preference.
- **The median is the headline number**, with mean and max reported beside it, because the alignment
  is fit to every frame at once and a single outlier therefore drags `G` slightly and smears a
  fraction of its error across every other frame. The median is the statistic an outlier cannot move.
- Input that cannot be scored is refused rather than answered: empty, mismatched lengths, or any
  quaternion that is not a rotation, in either set.

**It is test support, not a utility.** It absorbs "how we decide whether registration is accurate",
which is a testing concern rather than a shipped one, and nothing in `core/src` calls it. When the
native bench client needs the same number it graduates to `utilities/`, with the ADR that move
deserves. Putting it in `utilities/` now would be adding a component to the shipped core for the
convenience of a test.

## Consequences

- **Two properties of gauge-quotienting will surprise someone, so they are documented in the header
  and pinned by tests rather than left to be rediscovered.**

  A one-frame reconstruction always scores zero. With a single pair there is always a `G` carrying
  the estimate exactly onto the truth, so the gauge absorbs everything and nothing is left. That is
  not a defect — it is what accuracy means when there is nothing to be accurate *relative to*, and it
  is a real constraint on how a synthetic dataset must be built: frames that overlap, not merely
  frames.

  Two frames split their relative error evenly. If the rotation between them is out by theta, each
  reports theta/2, because neither is more wrong than the other and the gauge takes the common half.

- **Power iteration was the obvious implementation and it does not converge where it matters.** It
  converges as `(lambda2/lambda1)^k` — fast exactly when the residuals agree and slow exactly when
  they do not. Measured over 5,000 trials per regime: exact estimates settled in 1 iteration, errors
  around 5 degrees in 7, and **wholly unrelated estimates exhausted a 200-iteration budget**, as did
  half-exact-half-garbage input. That second regime is not a curiosity; it is what a half-broken
  registration looks like, which is precisely when a harness is being consulted. Jacobi's sweep count
  does not depend on the eigenvalue gap: measured maximum **6 sweeps** across every regime including
  the adversarial ones, against a budget of 24.

- **The metric is chordal, not angular, and they are not the same.** Maximising the sum of squared
  dot products is not minimising the sum of squared *angles*. The two agree to second order, so they
  diverge only once errors are large — and at that point the median is the number being read anyway,
  and the answer is "the registration is broken" under either metric. Stated here because a future
  reader comparing against a paper that minimises geodesic distance will get slightly different
  alignments and should know why.

- **A cost this accepts: the score is relative, so it cannot detect an absolute-orientation error.**
  If a future feature needs the panorama's rotation with respect to gravity or north — a `GPano`
  export with a real heading, say — that is a different measurement and this instrument says nothing
  about it. Sensor priors make it answerable; nothing needs it yet.

- **Two of the tests written for this were satisfied by their arrangement rather than by the
  behaviour, and both were found by sabotage rather than by reading.** The even-count median test
  asserted `EXPECT_NE` between the two middle values to show averaging was doing work; the values
  differed by 1e-12, so the assertion held under either implementation and removing the averaging
  broke nothing. The optimality test applied no gauge, which left the true answer 0.29 degrees from
  the identity — inside the resolution of its own 0.5-degree probe — so hardcoding the alignment to
  the identity passed it. Both are fixed and both now carry the reason in a comment. The general
  lesson, worth more than either fix: `EXPECT_NE` on floating point is nearly always true and says
  nothing about whether a difference is meaningful, and a local optimality probe proves nothing
  unless the optimum is further away than the probe is wide.

## Rejected

***Comparing frame rotations directly, with no alignment.*** Simpler, and it answers a different
question than the one asked: it measures the coordinate system the reconstruction happens to land
in. A perfect reconstruction scores arbitrarily badly, so every threshold would have had to be set
loose enough to admit that — which is the same as having no threshold.

***Scoring relative rotations between consecutive frames instead.*** Gauge-free by construction, and
genuinely appealing. Rejected because it hides the failure mode Phase 2 most needs to see: drift.
A sequence where each consecutive pair is accurate to 0.1 degrees can close the loop 5 degrees out,
and pairwise scoring calls that excellent. Aligning the whole set and measuring globally reports the
drift, which is the number a panorama's seams actually care about.

***Fixing the gauge by pinning the first frame to its truth.*** Cheap, and it makes frame zero's
error zero by definition — so a dataset whose first frame happened to register badly would report
that error smeared across all the others, and reordering the dataset would change the score. An
alignment fitted to every frame has no such arbitrary anchor.

***A full SVD-based orthogonal Procrustes solution on 3x3 rotation matrices.*** The textbook answer,
and equivalent up to the metric discussed above. Rejected because it needs an SVD the core does not
have — OpenCV could supply one, but that would tie the scorer to `SPHANORAMA_WITH_OPENCV` for
arithmetic that is forty lines of Jacobi, and the harness is the last place to want an optional
dependency.
