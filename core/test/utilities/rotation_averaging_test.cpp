#include "utilities/rotation_averaging.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

#include "support/rotation_scoring.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr double kDegPerRad = 180.0 / std::numbers::pi;

// Twelve frames is what the accuracy dataset renders and what a ring of this lens plans, so the
// solver is exercised at the size it will actually be asked to work at rather than at two.
constexpr int kFrames = 12;

Quat AboutY(double degrees) { return FromAxisAngle(Vec3{0, 1, 0}, degrees / kDegPerRad); }

double SeparationDeg(const Quat& a, const Quat& b) { return AngleBetween(a, b) * kDegPerRad; }

// A ring of frames turning about `y`, which is what a phone sweeping the horizon produces.
std::vector<Quat> Ring(int frames) {
  std::vector<Quat> truth;
  truth.reserve(static_cast<size_t>(frames));
  for (int i = 0; i < frames; ++i) {
    truth.push_back(AboutY(360.0 * i / frames));
  }
  return truth;
}

// A ring whose steps are **not** all the same size, which the one above cannot substitute for.
//
// A uniform ring is symmetric about every frame, and that symmetry hides an inverted edge
// convention completely: read the wrong way round, frame `i`'s two neighbours predict where frames
// `i-2` and `i+2` sit, and on an evenly spaced ring the average of those two is frame `i` again —
// exactly the right answer, by accident. Measured: with the convention inverted in the sweep,
// `ConsistentEdgesAndTruthfulAnchorsReproduceTheTruth` still passes.
//
// Uneven steps break the symmetry, so the two wrong predictions no longer average back to the truth.
// The angles are arbitrary and add to 360 so the ring still closes.
std::vector<Quat> UnevenRing() {
  const double steps[] = {17.0, 41.0, 8.0, 63.0, 22.0, 35.0, 51.0, 12.0, 29.0, 44.0, 19.0, 19.0};
  std::vector<Quat> truth;
  double at = 0;
  for (const double step : steps) {
    truth.push_back(AboutY(at));
    at += step;
  }
  return truth;
}

// The relative rotation `IRegistrationEngine` would answer with for this pair, exactly.
//
// Spelled from the convention in the header rather than reused from the solver, so the two are
// independent: a solver that inverted the convention and a fixture that inverted it with them would
// agree with each other and with nothing else.
Quat TrueEdge(const Quat& from, const Quat& to) {
  return Normalize(Multiply(Conjugate(to), from));
}

// Every consecutive pair of a ring, **and the pair that closes it**. The closing edge is the whole
// point: chaining throws it away, and it is the only measurement that says how much drift a chain
// accumulated.
std::vector<RelativeRotation> RingEdges(const std::vector<Quat>& truth, double biasDeg) {
  const Quat bias = AboutY(biasDeg);
  std::vector<RelativeRotation> edges;
  for (size_t i = 0; i < truth.size(); ++i) {
    const size_t next = (i + 1) % truth.size();
    edges.push_back(RelativeRotation{static_cast<int32_t>(i), static_cast<int32_t>(next),
                                     Normalize(Multiply(TrueEdge(truth[i], truth[next]), bias)),
                                     1.0});
  }
  return edges;
}

// What a chain does with the same edges, for the comparison the header's justification rests on.
// It walks the consecutive edges from frame zero and never reads the closing one.
std::vector<Quat> Chained(const std::vector<Quat>& truth,
                          const std::vector<RelativeRotation>& edges) {
  std::vector<Quat> chained{truth.front()};
  for (size_t i = 0; i + 1 < truth.size(); ++i) {
    chained.push_back(Normalize(Multiply(chained.back(), Conjugate(edges[i].rotation))));
  }
  return chained;
}

/**
 * Exact edges and exact anchors give back exactly the rotations they were built from.
 *
 * The weakest thing the solver can be asked and the one every other case is read against: the input
 * is already a fixed point, every prediction for every frame agrees, and anything that moves is the
 * solver moving for reasons of its own.
 *
 * **What it cannot see is an inverted edge convention**, and that is a property of the fixture
 * rather than of the assertion. Found by sabotage: reading every edge the wrong way round leaves
 * this test green, because a uniform ring is symmetric about each frame and the two wrong
 * predictions average back onto the right answer. `AnUnevenRingCatchesAnInvertedEdgeConvention` is
 * the one that does see it.
 */
TEST(AverageRotations, ConsistentEdgesAndTruthfulAnchorsReproduceTheTruth) {
  const std::vector<Quat> truth = Ring(kFrames);
  const std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);

  const AveragedRotations solved = AverageRotations(edges, truth, 0.01);
  ASSERT_TRUE(solved.valid);
  ASSERT_TRUE(solved.unplaced.empty());
  ASSERT_EQ(solved.rotations.size(), truth.size());
  EXPECT_TRUE(solved.converged);

  for (size_t i = 0; i < truth.size(); ++i) {
    EXPECT_NEAR(SeparationDeg(solved.rotations[i], truth[i]), 0.0, 1e-9) << "frame " << i;
  }
  EXPECT_NEAR(solved.maxEdgeErrorDeg, 0.0, 1e-9);
}

/**
 * Exact edges pull a set of anchors that are three degrees out back onto the truth.
 *
 * Three degrees is the order a fused phone orientation is wrong by when it is working — it is the
 * perturbation `registration_accuracy_test.cpp` uses, for the same reason — so this is the solver
 * being asked to do the job it exists for: the anchors say roughly where the frames are, the edges
 * say precisely how they sit relative to each other, and the answer should be the second with the
 * gauge taken from the first.
 *
 * Scored gauge-free, because the answer is determined only up to a common rotation and the anchors
 * fix that common rotation imprecisely — comparing frame by frame would report the anchors' shared
 * error on every frame at once, which is exactly the confusion `rotation_scoring` exists to remove.
 *
 * **The answer is not the truth, and the residual has a cause worth pinning.** At `anchorWeight`
 * 0.01 each frame's average is two exact edge predictions against one anchor that is three degrees
 * wrong, so about a two-hundredth of that error survives — measured, 0.0477 degrees of median
 * against the anchors' own 2.97, a factor of 62. Turning the anchors down to a millionth takes it to
 * 0.0000226, three and a half orders down, which is what says the 0.0477 is the anchors being
 * believed rather than the solver being approximate.
 *
 * **What 0.0000226 is *not* is the solver's precision**, and an earlier version of this paragraph
 * said it was. Run to the fixed point it is 4.83e-6: about four fifths of what this asserts is the
 * stopping rule, not the anchors. The conclusion above survives because 0.0477 is two thousand times
 * the stopping floor and cannot be explained by it — but the small number is where `kSettledDeg`
 * stops, and reading it as anything else gets the cause wrong. Found by a reviewer making the
 * tolerance tunable and running the same fixture down a decade.
 *
 * An earlier version also asserted the median below 1e-6 at `anchorWeight` 0.01, which assumed the
 * anchors contribute nothing — the thing they are passed in order to do.
 */
TEST(AverageRotations, ExactEdgesRecoverTheTruthFromAnchorsThatAreDegreesOut) {
  const std::vector<Quat> truth = Ring(kFrames);
  const std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);

  // A different axis per frame, so the perturbation is not one common rotation the gauge would
  // absorb for free — which would let a solver that ignored the edges entirely pass this.
  std::vector<Quat> anchors;
  for (int i = 0; i < kFrames; ++i) {
    const Vec3 axis{std::sin(i * 1.0), std::cos(i * 1.0), std::sin(i * 2.0)};
    anchors.push_back(
        Normalize(Multiply(truth[static_cast<size_t>(i)], FromAxisAngle(axis, 3.0 / kDegPerRad))));
  }

  const test::RotationScore before = test::ScoreRotations(anchors, truth);
  ASSERT_TRUE(before.valid);
  EXPECT_NEAR(before.medianDeg, 2.971980, 1e-5) << "the anchors are not the ones these figures come from";

  const AveragedRotations believed = AverageRotations(edges, anchors, 0.01);
  ASSERT_TRUE(believed.valid);
  EXPECT_TRUE(believed.converged);
  EXPECT_EQ(believed.sweeps, 590);

  const test::RotationScore after = test::ScoreRotations(believed.rotations, truth);
  ASSERT_TRUE(after.valid);
  std::fprintf(stderr, "[averaging] anchors median=%.4f deg; solved median=%.6f deg in %d sweeps\n",
               before.medianDeg, after.medianDeg, believed.sweeps);
  EXPECT_NEAR(after.medianDeg, 0.047665, 1e-5);
  EXPECT_NEAR(after.maxDeg, 0.050549, 1e-5);

  // The same edges and the same anchors, with the anchors weighed at a millionth instead of a
  // hundredth. If the residual above were the solver rather than the anchors, this would not move.
  const AveragedRotations barely = AverageRotations(edges, anchors, 1e-6);
  ASSERT_TRUE(barely.valid);
  EXPECT_TRUE(barely.converged);
  const test::RotationScore trusted = test::ScoreRotations(barely.rotations, truth);
  ASSERT_TRUE(trusted.valid);
  EXPECT_NEAR(trusted.medianDeg, 0.0000226, 1e-6);
}

/**
 * The closing edge is what removes a chain's drift, and removing one edge puts all of it back.
 *
 * This is the claim the header rests on, so it is a controlled comparison rather than an assertion.
 * Every edge carries the same 0.2-degree bias, the closing one included, so a chain around the ring
 * arrives 2.4 degrees from where it started and its worst frame is 1.1 degrees out. The chain is
 * then handed to the solver *as the anchors*, so both arms start from the identical drifted
 * estimate, and the only difference between them is whether the twelfth edge is in the input.
 *
 * With it, the answer comes back on the truth. Without it, it does not move at all — an open chain
 * is already a fixed point of this iteration, which is why the arm that loses the edge settles in a
 * single sweep and reports exactly the drift it was given.
 *
 * **Why the answer is the truth exactly, rather than merely better.** A bias that is the same on
 * every edge of a closed ring is a drift the closure measures in full: twelve steps each claiming
 * 0.2 degrees too much cannot all be right when the ring has to close, and the compromise that
 * disagrees with each of them equally is the one that turns 30 degrees per step. So every edge is
 * left 0.2 degrees unsatisfied and every frame lands where it started. That is a property of a
 * *uniform* bias and not of the solver being exact — `TheEdgeErrorReportsWhatTheAnswerLeaves
 * Unsatisfied` is the companion that reads the 0.2 back out.
 */
TEST(AverageRotations, TheClosingEdgeIsWhatRemovesAChainsDrift) {
  const std::vector<Quat> truth = Ring(kFrames);
  const std::vector<RelativeRotation> edges = RingEdges(truth, 0.2);
  const std::vector<Quat> chained = Chained(truth, edges);

  const test::RotationScore drifted = test::ScoreRotations(chained, truth);
  ASSERT_TRUE(drifted.valid);
  EXPECT_NEAR(drifted.maxDeg, 1.100000, 1e-6) << "the chain's worst frame has moved";
  EXPECT_NEAR(drifted.medianDeg, 0.600000, 1e-6);

  // The anchors weigh a millionth: enough to be the starting point, far too little to be the thing
  // correcting the frames. Without that this would be measuring how good the anchors are.
  const AveragedRotations closed = AverageRotations(edges, chained, 1e-6);
  ASSERT_TRUE(closed.valid);
  EXPECT_TRUE(closed.converged);
  const test::RotationScore recovered = test::ScoreRotations(closed.rotations, truth);
  ASSERT_TRUE(recovered.valid);
  std::fprintf(stderr, "[averaging] drift max=%.4f deg; closed max=%.7f deg in %d sweeps\n",
               drifted.maxDeg, recovered.maxDeg, closed.sweeps);

  // **0.0000278 is where the solver stops, not what it converges to.** Run to the fixed point the
  // worst frame is 2.4e-6 degrees — this figure is `kSettledDeg` showing through, and it moves if
  // that constant does. It is asserted anyway, and published anyway, because the claim it supports
  // is a comparison across five orders of magnitude: 1.100000 against 0.0000278 says the same thing
  // about the closing edge that 1.100000 against 0.0000024 would. What it must not be read as is the
  // precision of the solve.

  // **Where else this pair is written.** Both figures are published outside this file, so they move
  // in one commit or not at all — the same rule the accuracy table keeps under the heading "Where
  // else the table is written" in `registration_accuracy_test.cpp`:
  //
  //   1. `docs/06-roadmap.md`, in the paragraph that says what the discarded closing edge costs.
  //   2. `CLAUDE.md`, in the `rotation_averaging` paragraph.
  //   3. This test, here, which is the only place either is asserted.
  //
  // Asserted rather than bounded, and the bound is why: this was `EXPECT_LT(recovered.maxDeg, 1e-4)`
  // — 3.6 times looser than the number it was published beside, so the published digits reached a
  // reader through an `fprintf` and nothing could fail on them. ADR 0060 settled that a figure this
  // repository publishes gets asserted where it is produced.
  EXPECT_NEAR(recovered.maxDeg, 0.0000278, 5e-7);

  // The identical input with the twelfth edge taken out. Nothing else changes.
  const std::vector<RelativeRotation> open(edges.begin(), edges.end() - 1);
  const AveragedRotations unclosed = AverageRotations(open, chained, 1e-6);
  ASSERT_TRUE(unclosed.valid);
  EXPECT_TRUE(unclosed.converged);
  EXPECT_EQ(unclosed.sweeps, 1) << "an open chain should already be a fixed point of the sweep";
  const test::RotationScore stuck = test::ScoreRotations(unclosed.rotations, truth);
  ASSERT_TRUE(stuck.valid);
  EXPECT_NEAR(stuck.maxDeg, 1.100000, 1e-6)
      << "without the closing edge the drift should survive untouched";
}

/**
 * The gauge follows the anchors: turn every anchor by one rotation and the whole answer turns with
 * it.
 *
 * The edges cannot decide this — they are relative, and a reconstruction turned bodily is the same
 * reconstruction — so if the answer did *not* follow the anchors, something other than the anchors
 * would be choosing which way is north, and that something would be an accident of the solver's
 * initialisation.
 */
TEST(AverageRotations, TurningEveryAnchorTurnsTheWholeAnswer) {
  const std::vector<Quat> truth = Ring(kFrames);
  const std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);
  const Quat turn = FromAxisAngle(Vec3{1, 2, 3}, 40.0 / kDegPerRad);

  // **Anchors that are three degrees out, not the truth**, and that is the difference between
  // testing the solver and testing the walk. With truthful anchors and exact edges the input is
  // already a fixed point, so the sweep changes nothing and the equivariance asserted below is the
  // breadth-first placement's rather than the solve's — a reviewer proved it by replacing the whole
  // sweep with `(void)average` and watching this test stay green. Perturbed anchors make the solver
  // do work, and the assertion below then says that work commutes with a common rotation.
  std::vector<Quat> anchors;
  for (int i = 0; i < kFrames; ++i) {
    const Vec3 axis{std::sin(i * 1.0), std::cos(i * 1.0), std::sin(i * 2.0)};
    anchors.push_back(
        Normalize(Multiply(truth[static_cast<size_t>(i)], FromAxisAngle(axis, 3.0 / kDegPerRad))));
  }
  std::vector<Quat> turned;
  for (const Quat& q : anchors) turned.push_back(Normalize(Multiply(turn, q)));

  const AveragedRotations plain = AverageRotations(edges, anchors, 0.01);
  const AveragedRotations moved = AverageRotations(edges, turned, 0.01);
  ASSERT_TRUE(plain.valid && moved.valid);
  ASSERT_TRUE(plain.converged && moved.converged);

  // The answer has to have moved off the anchors, or a solver that returned its input unchanged
  // would satisfy the equivariance below for free.
  double worstMove = 0;
  for (size_t i = 0; i < anchors.size(); ++i) {
    worstMove = std::max(worstMove, SeparationDeg(plain.rotations[i], anchors[i]));
  }
  EXPECT_GT(worstMove, 1.0) << "the solve left the anchors where they were";

  // 1e-4 rather than 1e-9: the two solves take 590 and 591 sweeps, so they stop at slightly
  // different points on the same trajectory. Measured at 9.8e-6 between them.
  for (size_t i = 0; i < anchors.size(); ++i) {
    const Quat expected = Normalize(Multiply(turn, plain.rotations[i]));
    EXPECT_NEAR(SeparationDeg(moved.rotations[i], expected), 0.0, 1e-4) << "frame " << i;
  }
}

/**
 * An `anchorWeight` of zero places the frames and then believes only the edges.
 *
 * Three paragraphs of the header describe this and, until a reviewer counted, no test passed it —
 * the file used 0.01, 1e-4, 1e-6, 1000 and the refusals. It is not a cosmetic gap: the "this call
 * cannot refuse" argument in the sweep rests on `anchorWeight > 0.0` keeping a zero-weight anchor
 * out of the prediction list, and relaxing that comparison to `>=` puts every frame **180 degrees**
 * from its anchor with `valid` and `converged` both true, because a lone prediction at weight zero
 * is a matrix the averager cannot read.
 *
 * What the case should do is recover the truth exactly: the anchors are three degrees out, they fix
 * the gauge by placing the frames, and the exact edges then decide everything else with nothing
 * pulling back toward the priors.
 */
TEST(AverageRotations, AnAnchorWeightOfZeroPlacesTheFramesAndIsNotConsultedAgain) {
  const std::vector<Quat> truth = Ring(kFrames);
  const std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);

  std::vector<Quat> anchors;
  for (int i = 0; i < kFrames; ++i) {
    const Vec3 axis{std::sin(i * 1.0), std::cos(i * 1.0), std::sin(i * 2.0)};
    anchors.push_back(
        Normalize(Multiply(truth[static_cast<size_t>(i)], FromAxisAngle(axis, 3.0 / kDegPerRad))));
  }

  const AveragedRotations solved = AverageRotations(edges, anchors, 0.0);
  ASSERT_TRUE(solved.valid);
  EXPECT_TRUE(solved.unplaced.empty()) << "the anchors did not place the frames";
  EXPECT_TRUE(solved.converged);
  EXPECT_EQ(solved.edgesUsed, kFrames);

  // Not zero, and the reason is the solver rather than the anchors: the sweep stops when the largest
  // per-frame move falls under `kSettledDeg`, which is 1e-5 degrees, so a residual a couple of times
  // that is where an exactly-solvable problem lands. Written as `1e-6` first, which assumed a solver
  // with no stopping rule — the same wrong assumption as the earlier `1e-6` on the perturbed-anchor
  // case, and a reminder that "exact edges" bounds the problem and not the iteration.
  const test::RotationScore score = test::ScoreRotations(solved.rotations, truth);
  ASSERT_TRUE(score.valid);
  EXPECT_NEAR(score.medianDeg, 0.0000286, 3e-6)
      << "the residual is no longer the stopping tolerance, so something else is moving the answer";

  // **The gauge is a compromise among all twelve anchors, not a copy of one of them**, and getting
  // that wrong is what this block is for. The assertion here was first written as "frame zero ends
  // where its own anchor put it", on the theory that the walk chains outward from a single seed. It
  // does not: it seeds *every* anchored frame at its own anchor, so the starting set is twelve
  // mutually inconsistent placements and the sweep relaxes them into one consistent reconstruction
  // whose gauge none of them chose. Measured, frame zero lands 3.31 degrees from its own anchor —
  // the scale of the perturbation, which is what a compromise among twelve three-degree errors
  // should cost.
  //
  // So the answer is on the truth up to a gauge (asserted above) and is not on any particular
  // anchor, and both halves are needed: the first alone is satisfied by a solver that ignores the
  // anchors, the second alone by one that ignores the edges.
  EXPECT_NEAR(SeparationDeg(solved.rotations[0], anchors[0]), 3.3088, 1e-3);

  // **And the same weight with no edges at all**, which is the case that makes `anchorWeight > 0.0`
  // load-bearing rather than tidy. With edges present a zero-weighted anchor is harmless — it enters
  // the prediction list beside two edge predictions and the averager ignores a zero. With no edges
  // it would be the *only* prediction, and a lone prediction at weight zero is a set the averager
  // refuses, leaving the frame on whatever a refused answer carries. Relaxing the comparison to
  // `>=` puts every frame 180 degrees from its anchor here, with `valid` and `converged` true.
  //
  // The right answer is the anchors, untouched: nothing was measured, so nothing should move.
  const AveragedRotations untouched = AverageRotations({}, anchors, 0.0);
  ASSERT_TRUE(untouched.valid);
  EXPECT_TRUE(untouched.converged);
  EXPECT_TRUE(untouched.unplaced.empty());
  EXPECT_EQ(untouched.edgesUsed, 0);
  for (size_t i = 0; i < anchors.size(); ++i) {
    EXPECT_NEAR(SeparationDeg(untouched.rotations[i], anchors[i]), 0.0, 1e-12) << "frame " << i;
  }
}

/**
 * An edge weighted at zero is not consulted, however wrong it is.
 *
 * What ADR 0056 leaves a caller holding: a pair that was estimated, was not accepted, and still
 * carries the best rotation the pixels offered. Weighing it at nothing has to be the same as not
 * having it — and the second half of this test is what makes the first half mean something, because
 * an edge that is ignored for any other reason would satisfy the first half alone.
 */
TEST(AverageRotations, AZeroWeightedEdgeIsNotConsulted) {
  const std::vector<Quat> truth = Ring(kFrames);
  std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);
  edges.push_back(RelativeRotation{0, 6, AboutY(90.0), 0.0});

  const AveragedRotations solved = AverageRotations(edges, truth, 0.01);
  ASSERT_TRUE(solved.valid);
  for (size_t i = 0; i < truth.size(); ++i) {
    EXPECT_NEAR(SeparationDeg(solved.rotations[i], truth[i]), 0.0, 1e-9) << "frame " << i;
  }

  // The same edge at weight one moves the answer a long way, so the zero is doing the work and not
  // the edge being harmless.
  edges.back().weight = 1.0;
  const AveragedRotations disturbed = AverageRotations(edges, truth, 0.01);
  ASSERT_TRUE(disturbed.valid);
  double worst = 0;
  for (size_t i = 0; i < truth.size(); ++i) {
    worst = std::max(worst, SeparationDeg(disturbed.rotations[i], truth[i]));
  }
  EXPECT_GT(worst, 5.0) << "the wrong edge does not disturb the answer even when it is counted";
}

/**
 * A frame no edge reaches and no anchor places is named, not silently handed the identity.
 *
 * The identity is a perfectly ordinary rotation — a phone held level reports it — so a caller
 * cannot tell one that was solved for from one that was given up on. It has to be told.
 */
TEST(AverageRotations, AFrameWithNoAnchorAndNoEdgeIsNamedUnplaced) {
  const std::vector<Quat> truth = Ring(kFrames);
  const std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);

  std::vector<Quat> anchors = truth;
  anchors.push_back(Quat{0, 0, 0, 0});   // a thirteenth frame the sensor never answered for

  const AveragedRotations solved = AverageRotations(edges, anchors, 0.01);
  ASSERT_TRUE(solved.valid);
  ASSERT_EQ(solved.rotations.size(), anchors.size());
  ASSERT_EQ(solved.unplaced.size(), 1u);
  EXPECT_EQ(solved.unplaced.front(), kFrames);
  EXPECT_NEAR(SeparationDeg(solved.rotations[kFrames], Quat{}), 0.0, 1e-12);

  // And the twelve that could be placed are unaffected by the one that could not.
  for (size_t i = 0; i < truth.size(); ++i) {
    EXPECT_NEAR(SeparationDeg(solved.rotations[i], truth[i]), 0.0, 1e-9) << "frame " << i;
  }
}

/**
 * A frame with no anchor that an edge does reach is solved for, and is not unplaced.
 *
 * The companion to the case above, and the one that says `unplaced` means "nothing reached it"
 * rather than "it had no anchor". A sensor that drops one sample should not cost a frame.
 */
TEST(AverageRotations, AFrameWithNoAnchorThatAnEdgeReachesIsStillSolvedFor) {
  const std::vector<Quat> truth = Ring(kFrames);
  const std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);

  std::vector<Quat> anchors = truth;
  anchors[5] = Quat{0, 0, 0, 0};

  const AveragedRotations solved = AverageRotations(edges, anchors, 0.01);
  ASSERT_TRUE(solved.valid);
  EXPECT_TRUE(solved.unplaced.empty());
  EXPECT_NEAR(SeparationDeg(solved.rotations[5], truth[5]), 0.0, 1e-9);
}

/**
 * With no edges at all the answer is the anchors, and nothing disagrees.
 *
 * Reachable the moment a caller has priors and no registered pair yet, and the honest answer is the
 * priors rather than a refusal — a refusal would say the input was unreadable, which it is not.
 * `medianEdgeErrorDeg` is zero because no edge was used, which is a different zero from "every edge
 * is satisfied"; the header says which.
 */
TEST(AverageRotations, NoEdgesLeavesTheAnchorsAloneAndReportsNoEdgeError) {
  const std::vector<Quat> truth = Ring(kFrames);
  const AveragedRotations solved = AverageRotations({}, truth, 0.01);
  ASSERT_TRUE(solved.valid);
  EXPECT_TRUE(solved.unplaced.empty());
  EXPECT_TRUE(solved.converged);
  for (size_t i = 0; i < truth.size(); ++i) {
    EXPECT_NEAR(SeparationDeg(solved.rotations[i], truth[i]), 0.0, 1e-12) << "frame " << i;
  }
  EXPECT_EQ(solved.medianEdgeErrorDeg, 0.0);
  EXPECT_EQ(solved.maxEdgeErrorDeg, 0.0);
}

/**
 * The reported edge error is the disagreement the solver actually left, not a constant.
 *
 * With every edge biased by the same 0.2 degrees the ring cannot close, so some disagreement has to
 * survive, and the number that comes back has to be it. Asserted as a value rather than "greater
 * than zero", which a solver that reported the bias itself would also satisfy.
 */
TEST(AverageRotations, TheEdgeErrorReportsWhatTheAnswerLeavesUnsatisfied) {
  const std::vector<Quat> truth = Ring(kFrames);
  const std::vector<RelativeRotation> edges = RingEdges(truth, 0.2);

  const AveragedRotations solved = AverageRotations(edges, truth, 1e-6);
  ASSERT_TRUE(solved.valid);
  EXPECT_NEAR(solved.medianEdgeErrorDeg, 0.200000, 1e-4);
  EXPECT_NEAR(solved.maxEdgeErrorDeg, 0.200000, 1e-4);
}

/**
 * An edge's weight decides how much of it survives, not merely whether it is used at all.
 *
 * Every other test here passes edges at one weight or at zero, which between them cannot tell a
 * weight from a flag. Two edges are offered between the same pair, disagreeing by twenty degrees,
 * with three to one between them — and the answer has to land where the weighted average puts it,
 * which about a common axis is `atan2(sum w sin a, sum w cos a)` and is 34.9616 degrees.
 *
 * Frame one carries no anchor, so the two edges are the only things speaking for it. Frame zero's
 * anchor is weighed at a thousand so that it stays where it is put and the measurement is about the
 * edges rather than about where both frames drifted to.
 */
TEST(AverageRotations, TwoEdgesThatDisagreeAreWeighedAgainstEachOther) {
  const std::vector<Quat> anchors{AboutY(0.0), Quat{0, 0, 0, 0}};
  const std::vector<RelativeRotation> edges{
      RelativeRotation{0, 1, TrueEdge(AboutY(0.0), AboutY(30.0)), 3.0},
      RelativeRotation{0, 1, TrueEdge(AboutY(0.0), AboutY(50.0)), 1.0},
  };

  const AveragedRotations solved = AverageRotations(edges, anchors, 1000.0);
  ASSERT_TRUE(solved.valid);
  ASSERT_TRUE(solved.unplaced.empty());
  EXPECT_TRUE(solved.converged);
  EXPECT_NEAR(SeparationDeg(solved.rotations[0], AboutY(0.0)), 0.0, 1e-6)
      << "the heavily anchored frame moved, so the figure below is not about the edges";
  EXPECT_NEAR(SeparationDeg(solved.rotations[1], AboutY(34.961631)), 0.0, 1e-5);

  // The same two edges at equal weight land at forty, which is what says the number above is the
  // three-to-one and not the pair.
  std::vector<RelativeRotation> even = edges;
  even[0].weight = 1.0;
  const AveragedRotations balanced = AverageRotations(even, anchors, 1000.0);
  ASSERT_TRUE(balanced.valid);
  EXPECT_NEAR(SeparationDeg(balanced.rotations[1], AboutY(40.0)), 0.0, 1e-5);
}

/**
 * A solve that runs out of sweeps says so, and still answers.
 *
 * `converged` is reported rather than promised, and a flag nothing can set to false is not a report.
 * Anchors at a ten-thousandth are what reach it: the sweeps needed scale as roughly one over the
 * anchor weight, because what is still moving at that point is the gauge — one common rotation
 * shared by every frame — and a weak anchor is a weak constraint on exactly that. Measured, a
 * twelve-frame ring at 1e-4 wants about 12,500 sweeps against a budget of 1,000.
 *
 * The second half is the half that matters to a caller: what comes back is still a good answer. The
 * budget ran out on the gauge, and the gauge is the part `ScoreRotations` removes.
 */
TEST(AverageRotations, ASolveThatRunsOutOfSweepsSaysSoAndStillAnswers) {
  const std::vector<Quat> truth = Ring(kFrames);
  const std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);

  std::vector<Quat> anchors;
  for (int i = 0; i < kFrames; ++i) {
    const Vec3 axis{std::sin(i * 1.0), std::cos(i * 1.0), std::sin(i * 2.0)};
    anchors.push_back(
        Normalize(Multiply(truth[static_cast<size_t>(i)], FromAxisAngle(axis, 3.0 / kDegPerRad))));
  }

  const AveragedRotations solved = AverageRotations(edges, anchors, 1e-4);
  ASSERT_TRUE(solved.valid);
  EXPECT_FALSE(solved.converged);
  EXPECT_EQ(solved.sweeps, 1000) << "the budget is not what stopped it";

  const test::RotationScore score = test::ScoreRotations(solved.rotations, truth);
  ASSERT_TRUE(score.valid);
  EXPECT_LT(score.medianDeg, 0.001) << "an unconverged answer is still the best the solver reached";
}

/**
 * A zero-weighted edge is left out of the reported error as well as out of the solve.
 *
 * Counting it would be the same defect in the other direction from consulting it: an edge nobody
 * asked the solver to satisfy would read as a disagreement the answer is responsible for, and a
 * caller reading `maxEdgeErrorDeg` to decide whether a reconstruction is trustworthy would see a
 * number about an edge it had already discarded.
 */
TEST(AverageRotations, AZeroWeightedEdgeIsNotCountedInTheReportedError) {
  const std::vector<Quat> truth = Ring(kFrames);
  std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);
  edges.push_back(RelativeRotation{0, 6, AboutY(90.0), 0.0});

  const AveragedRotations solved = AverageRotations(edges, truth, 0.01);
  ASSERT_TRUE(solved.valid);
  EXPECT_NEAR(solved.maxEdgeErrorDeg, 0.0, 1e-9);

  // The same edge counted, so the zero is what keeps it out rather than the edge agreeing.
  edges.back().weight = 1.0;
  const AveragedRotations counted = AverageRotations(edges, truth, 0.01);
  ASSERT_TRUE(counted.valid);
  EXPECT_GT(counted.maxEdgeErrorDeg, 10.0);
}

/**
 * A ring with uneven steps catches an edge convention read the wrong way round.
 *
 * The companion to the blind spot named on `ConsistentEdgesAndTruthfulAnchorsReproduceTheTruth`, and
 * the reason a second ring fixture exists at all. The input is the same shape — consistent edges,
 * truthful anchors, a closed ring — and the only difference is that the steps are not all equal, so
 * the symmetry that made two wrong predictions cancel is gone.
 *
 * Measured: with the convention inverted in the sweep this reports tens of degrees of error, where
 * the uniform ring reports none.
 */
TEST(AverageRotations, AnUnevenRingCatchesAnInvertedEdgeConvention) {
  const std::vector<Quat> truth = UnevenRing();
  const std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);

  const AveragedRotations solved = AverageRotations(edges, truth, 0.01);
  ASSERT_TRUE(solved.valid);
  EXPECT_TRUE(solved.converged);
  for (size_t i = 0; i < truth.size(); ++i) {
    EXPECT_NEAR(SeparationDeg(solved.rotations[i], truth[i]), 0.0, 1e-9) << "frame " << i;
  }
  EXPECT_NEAR(solved.maxEdgeErrorDeg, 0.0, 1e-9);
}

/**
 * A frame an edge reaches *only* at weight zero is unplaced, not placed by the edge it discarded.
 *
 * This is the job the weight filter actually does, and nothing else was testing it: the sweep would
 * ignore a zero-weighted edge anyway, because the averager is handed the weight and a zero removes
 * a prediction there too. So `AZeroWeightedEdgeIsNotConsulted` passes with the filter deleted —
 * verified by sabotage — and the only thing that notices is the placement walk, which has no weights
 * and would otherwise put a frame somewhere on the strength of a measurement the caller discarded.
 */
TEST(AverageRotations, AFrameReachedOnlyByAZeroWeightedEdgeIsUnplaced) {
  const std::vector<Quat> truth = Ring(kFrames);
  std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);

  std::vector<Quat> anchors = truth;
  anchors.push_back(Quat{0, 0, 0, 0});
  edges.push_back(RelativeRotation{0, kFrames, TrueEdge(truth[0], AboutY(75.0)), 0.0});

  const AveragedRotations discarded = AverageRotations(edges, anchors, 0.01);
  ASSERT_TRUE(discarded.valid);
  ASSERT_EQ(discarded.unplaced.size(), 1u);
  EXPECT_EQ(discarded.unplaced.front(), kFrames);
  EXPECT_NEAR(SeparationDeg(discarded.rotations[kFrames], Quat{}), 0.0, 1e-12);

  // The same edge believed places the frame where it says, so the zero is what withheld it.
  edges.back().weight = 1.0;
  const AveragedRotations believed = AverageRotations(edges, anchors, 0.01);
  ASSERT_TRUE(believed.valid);
  EXPECT_TRUE(believed.unplaced.empty());
  EXPECT_NEAR(SeparationDeg(believed.rotations[kFrames], AboutY(75.0)), 0.0, 1e-6);
}

/**
 * Zero disagreement over no edges is told apart from zero disagreement over eleven.
 *
 * The two report the identical pair of numbers — `medianEdgeErrorDeg` and `maxEdgeErrorDeg` both
 * zero, `valid` true, `converged` true — and they are not the same fact. The first is a
 * reconstruction the pixels never touched; the second is one every measurement agrees with. A caller
 * writing `valid && maxEdgeErrorDeg < 0.5` to decide whether to trust a sphere accepts the first.
 *
 * `edgesUsed` is the denominator that separates them, and it exists because a reviewer found the
 * numerator published without one.
 */
TEST(AverageRotations, TheEdgeErrorCarriesTheCountItWasComputedOver) {
  const std::vector<Quat> truth = Ring(kFrames);
  const std::vector<RelativeRotation> satisfied = RingEdges(truth, 0.0);

  const AveragedRotations solved = AverageRotations(satisfied, truth, 0.01);
  ASSERT_TRUE(solved.valid);
  EXPECT_EQ(solved.edgesUsed, kFrames);
  EXPECT_NEAR(solved.maxEdgeErrorDeg, 0.0, 1e-9);

  // Every edge discarded. The error figures are identical and the count is what says why.
  std::vector<RelativeRotation> discarded = satisfied;
  for (RelativeRotation& edge : discarded) edge.weight = 0.0;
  const AveragedRotations priorsOnly = AverageRotations(discarded, truth, 0.01);
  ASSERT_TRUE(priorsOnly.valid);
  EXPECT_TRUE(priorsOnly.converged);
  EXPECT_NEAR(priorsOnly.maxEdgeErrorDeg, 0.0, 1e-9)
      << "the two cases have to be indistinguishable on this field, or the test proves nothing";
  EXPECT_NEAR(priorsOnly.medianEdgeErrorDeg, 0.0, 1e-9);
  EXPECT_EQ(priorsOnly.edgesUsed, 0);

  // And an edge whose endpoints were never placed is not counted either, which is the second
  // exclusion the header names. Frames 12 and 13 have no anchor and are reachable only from each
  // other, so the edge between them is believed and never used.
  std::vector<Quat> anchors = truth;
  anchors.push_back(Quat{0, 0, 0, 0});
  anchors.push_back(Quat{0, 0, 0, 0});
  std::vector<RelativeRotation> stranded = satisfied;
  stranded.push_back(RelativeRotation{kFrames, kFrames + 1, AboutY(90.0), 1.0});

  const AveragedRotations partial = AverageRotations(stranded, anchors, 0.01);
  ASSERT_TRUE(partial.valid);
  EXPECT_EQ(partial.unplaced.size(), 2u);
  EXPECT_EQ(partial.edgesUsed, kFrames) << "the stranded edge was counted";
  EXPECT_NEAR(partial.maxEdgeErrorDeg, 0.0, 1e-9);

  // **And the stranded pair still holds the identity**, which is what the sweep's own `placed`
  // guards are for and what nothing else here asserts. A component with no anchor in it is the input
  // that makes them load-bearing — every other unplaced case in this file is a single isolated frame
  // with no edge to be moved by — and without them frames 12 and 13 would be averaged against each
  // other's identities and drift away from it while `unplaced` went on naming them.
  EXPECT_NEAR(SeparationDeg(partial.rotations[kFrames], Quat{}), 0.0, 1e-12);
  EXPECT_NEAR(SeparationDeg(partial.rotations[kFrames + 1], Quat{}), 0.0, 1e-12);
}

/**
 * The median edge error is a median, over errors that are not all the same.
 *
 * Every other case here leaves every edge equally satisfied — exactly, or by the same uniform bias —
 * so `medianEdgeErrorDeg` and `maxEdgeErrorDeg` carry the same number and returning the first
 * element, or the mean, or the maximum would satisfy all of them. A reviewer showed that
 * `errors.front()` passes the whole suite.
 *
 * Three edges with three different errors, and an even-count case beside it, because the even branch
 * averages the two middle values and is a second thing that can be wrong on its own.
 */
TEST(AverageRotations, TheMedianEdgeErrorIsAMedianOverErrorsThatDiffer) {
  // A chain of four frames, anchored hard at every one so the solve leaves them where they are and
  // the edge errors are whatever the edges disagree with the anchors by. That makes the three
  // residuals knowable in advance rather than a property of the iteration.
  const std::vector<Quat> anchors{AboutY(0.0), AboutY(30.0), AboutY(60.0), AboutY(90.0)};
  const std::vector<RelativeRotation> edges{
      RelativeRotation{0, 1, TrueEdge(AboutY(0.0), AboutY(32.0)), 1.0},   // 2 degrees out
      RelativeRotation{1, 2, TrueEdge(AboutY(30.0), AboutY(68.0)), 1.0},  // 8 degrees out
      RelativeRotation{2, 3, TrueEdge(AboutY(60.0), AboutY(94.0)), 1.0},  // 4 degrees out
  };

  const AveragedRotations solved = AverageRotations(edges, anchors, 1e6);
  ASSERT_TRUE(solved.valid);
  ASSERT_EQ(solved.edgesUsed, 3);
  EXPECT_NEAR(solved.maxEdgeErrorDeg, 8.0, 1e-3);
  EXPECT_NEAR(solved.medianEdgeErrorDeg, 4.0, 1e-3)
      << "2, 4 and 8 have a median of 4 — the first, the mean and the max are all something else";

  // Even count: drop the last edge and the median is the mean of the two that remain.
  const std::vector<RelativeRotation> two(edges.begin(), edges.begin() + 2);
  const AveragedRotations pair = AverageRotations(two, anchors, 1e6);
  ASSERT_TRUE(pair.valid);
  ASSERT_EQ(pair.edgesUsed, 2);
  EXPECT_NEAR(pair.medianEdgeErrorDeg, 5.0, 1e-3) << "2 and 8 average to 5, they do not pick one";
}

/**
 * A frame whose evidence cancels is named, not silently placed by the eigensolver's scan order.
 *
 * `AverageQuaternions` reports when the top two eigenvalues tie, which means every rotation in a
 * whole plane maximises the objective equally and the one returned is decided by the order the
 * diagonal was scanned in. The solver computed that flag on every frame of every sweep and threw it
 * away, so a frame placed by a coin flip came back looking exactly like one the edges agreed on.
 *
 * Reachable rather than theoretical: two neighbours predicting orientations a half turn apart is a
 * registration that has gone badly wrong, which is precisely when a caller wants to be told. Frame 1
 * here has no anchor and two edges saying it is at 0 and at 180 degrees.
 */
TEST(AverageRotations, AFrameWhoseEvidenceCancelsIsNamedAmbiguous) {
  const std::vector<Quat> anchors{AboutY(0.0), Quat{0, 0, 0, 0}, AboutY(0.0)};
  const std::vector<RelativeRotation> opposed{
      RelativeRotation{0, 1, TrueEdge(AboutY(0.0), AboutY(0.0)), 1.0},
      RelativeRotation{2, 1, TrueEdge(AboutY(0.0), AboutY(180.0)), 1.0},
  };

  const AveragedRotations solved = AverageRotations(opposed, anchors, 1000.0);
  ASSERT_TRUE(solved.valid);
  EXPECT_TRUE(solved.unplaced.empty()) << "frame 1 was reachable, so it is placed and merely unsure";
  ASSERT_EQ(solved.ambiguous.size(), 1u);
  EXPECT_EQ(solved.ambiguous.front(), 1);

  // The same shape with the two edges agreeing leaves nobody ambiguous, so the flag is reporting the
  // cancellation rather than the topology.
  const std::vector<RelativeRotation> agreed{
      RelativeRotation{0, 1, TrueEdge(AboutY(0.0), AboutY(0.0)), 1.0},
      RelativeRotation{2, 1, TrueEdge(AboutY(0.0), AboutY(0.0)), 1.0},
  };
  const AveragedRotations settled = AverageRotations(agreed, anchors, 1000.0);
  ASSERT_TRUE(settled.valid);
  EXPECT_TRUE(settled.ambiguous.empty());
  EXPECT_NEAR(SeparationDeg(settled.rotations[1], AboutY(0.0)), 0.0, 1e-9);
}

// ----------------------------------------------------------------- refusals
//
// Asserted one at a time rather than in a loop over "bad inputs", because a loop proves that
// something refused and not that the right thing did.

TEST(AverageRotations, NoFramesIsARefusal) {
  EXPECT_FALSE(AverageRotations({}, {}, 0.01).valid);
}

/**
 * More frames than an `int32_t` can name is a refusal, not a cast.
 *
 * `anchors.size()` is a `size_t` and the frame index is an `int32_t`, so the conversion between them
 * is where this stops being arithmetic. A reviewer reproduced both ends on a `MAP_NORESERVE`
 * mapping, which hands out a span of any length for no physical pages: at `2^31` the cast wraps
 * negative and the first allocation throws, which under `-fno-exceptions` is `std::terminate` rather
 * than a refusal; at `2^32 + 3` it wraps to 3 and answers `valid == true` over a silently truncated
 * solve whose `rotations` is not parallel to its `anchors`.
 *
 * The test cannot allocate that, so it asserts the gate rather than the wrap: a span constructed over
 * a null pointer with a length past `INT32_MAX` is refused before anything reads it. That is a real
 * span of that length — `std::span` does no dereferencing to be built — and the refusal happens on
 * the first two lines of the function, which is the whole claim.
 */
TEST(AverageRotations, MoreFramesThanAnIndexCanNameIsARefusal) {
  const size_t tooMany = static_cast<size_t>(std::numeric_limits<int32_t>::max()) + 1;
  const std::span<const Quat> unreadable(static_cast<const Quat*>(nullptr), tooMany);
  EXPECT_FALSE(AverageRotations({}, unreadable, 0.01).valid);

  // And the same on the edge list, which is counted into an `int32_t` for `Incidence::edge`.
  const std::vector<Quat> truth = Ring(kFrames);
  const std::span<const RelativeRotation> tooManyEdges(
      static_cast<const RelativeRotation*>(nullptr),
      static_cast<size_t>(std::numeric_limits<int32_t>::max()) + 1);
  EXPECT_FALSE(AverageRotations(tooManyEdges, truth, 0.01).valid);
}

TEST(AverageRotations, AnEdgeNamingAFrameThatIsNotThereIsARefusal) {
  const std::vector<Quat> truth = Ring(kFrames);
  std::vector<RelativeRotation> tooHigh = RingEdges(truth, 0.0);
  tooHigh.push_back(RelativeRotation{0, kFrames, AboutY(10.0), 1.0});
  EXPECT_FALSE(AverageRotations(tooHigh, truth, 0.01).valid);

  std::vector<RelativeRotation> negative = RingEdges(truth, 0.0);
  negative.push_back(RelativeRotation{-1, 0, AboutY(10.0), 1.0});
  EXPECT_FALSE(AverageRotations(negative, truth, 0.01).valid);
}

/**
 * An out-of-range edge is refused even at weight zero.
 *
 * The weight says how much an edge is believed, which is a question about a measurement; an index
 * that names no frame is not a measurement at all. Refusing it only when it is consulted would make
 * the same malformed input readable or not depending on a number beside it — and would leave the
 * bounds check load-bearing on the weight rather than on the index.
 */
TEST(AverageRotations, AnOutOfRangeEdgeIsRefusedEvenAtWeightZero) {
  const std::vector<Quat> truth = Ring(kFrames);
  std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);
  edges.push_back(RelativeRotation{0, kFrames + 4, AboutY(10.0), 0.0});
  EXPECT_FALSE(AverageRotations(edges, truth, 0.01).valid);
}

/**
 * An edge from a frame to itself is a refusal.
 *
 * `conjugate(q) * q` is the identity whatever `q` is, so a self edge says either nothing at all or
 * something impossible — and the solver would read it as a prediction that frame `i` should be
 * `q[i] * rotation`, which pulls the frame a fixed amount every sweep and never settles. It is not a
 * measurement to disbelieve at a low weight; it is not a measurement.
 */
TEST(AverageRotations, AnEdgeFromAFrameToItselfIsARefusal) {
  const std::vector<Quat> truth = Ring(kFrames);
  std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);
  edges.push_back(RelativeRotation{4, 4, AboutY(10.0), 1.0});
  EXPECT_FALSE(AverageRotations(edges, truth, 0.01).valid);
}

TEST(AverageRotations, AnEdgeWhoseRotationIsNotOneIsARefusal) {
  const std::vector<Quat> truth = Ring(kFrames);
  std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);
  edges[3].rotation = Quat{0, 0, 0, 0};
  EXPECT_FALSE(AverageRotations(edges, truth, 0.01).valid);
}

TEST(AverageRotations, AnEdgeWeightThatIsNotAMeasurementIsARefusal) {
  const std::vector<Quat> truth = Ring(kFrames);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double infinity = std::numeric_limits<double>::infinity();

  std::vector<RelativeRotation> negative = RingEdges(truth, 0.0);
  negative[2].weight = -1.0;
  EXPECT_FALSE(AverageRotations(negative, truth, 0.01).valid);

  std::vector<RelativeRotation> notANumber = RingEdges(truth, 0.0);
  notANumber[2].weight = nan;
  EXPECT_FALSE(AverageRotations(notANumber, truth, 0.01).valid);

  std::vector<RelativeRotation> unbounded = RingEdges(truth, 0.0);
  unbounded[2].weight = infinity;
  EXPECT_FALSE(AverageRotations(unbounded, truth, 0.01).valid);
}

TEST(AverageRotations, AnAnchorWeightThatIsNotAMeasurementIsARefusal) {
  const std::vector<Quat> truth = Ring(kFrames);
  const std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);
  EXPECT_FALSE(AverageRotations(edges, truth, -1.0).valid);
  EXPECT_FALSE(AverageRotations(edges, truth, std::numeric_limits<double>::quiet_NaN()).valid);
  EXPECT_FALSE(AverageRotations(edges, truth, std::numeric_limits<double>::infinity()).valid);
}

/**
 * No usable anchor anywhere is a refusal, and it is the gauge that makes it one.
 *
 * With edges alone the answer is determined only up to a common rotation, so every reconstruction
 * is as good as every other and the one that would come back is whatever the solver's
 * initialisation happened to start from — an arbitrary choice dressed as a measurement. The same
 * input with a single usable anchor is answerable, which is the second half of this test and is
 * what says the refusal is about the gauge rather than about the anchors being present.
 */
TEST(AverageRotations, NoUsableAnchorAnywhereIsARefusal) {
  const std::vector<Quat> truth = Ring(kFrames);
  const std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);

  std::vector<Quat> none(static_cast<size_t>(kFrames), Quat{0, 0, 0, 0});
  EXPECT_FALSE(AverageRotations(edges, none, 0.01).valid);

  std::vector<Quat> one = none;
  one[7] = truth[7];
  const AveragedRotations solved = AverageRotations(edges, one, 0.01);
  ASSERT_TRUE(solved.valid);
  EXPECT_TRUE(solved.unplaced.empty());
  for (size_t i = 0; i < truth.size(); ++i) {
    EXPECT_NEAR(SeparationDeg(solved.rotations[i], truth[i]), 0.0, 1e-9) << "frame " << i;
  }
}

/**
 * An anchor span shorter than the frames the edges name is a refusal.
 *
 * `anchors` is what says how many frames there are, so an edge naming a frame beyond it is the
 * out-of-range case above. This is the same fact asserted from the other side: there is no frame
 * count hiding anywhere else that the two could be checked against separately.
 */
TEST(AverageRotations, EdgesReachingPastTheAnchorsAreRefused) {
  const std::vector<Quat> truth = Ring(kFrames);
  const std::vector<RelativeRotation> edges = RingEdges(truth, 0.0);
  const std::vector<Quat> shorter(truth.begin(), truth.begin() + 4);
  EXPECT_FALSE(AverageRotations(edges, shorter, 0.01).valid);
}

}  // namespace
}  // namespace sphanorama
