#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "sphanorama/types.h"

namespace sphanorama {

// One consistent set of absolute rotations from many independently measured relative ones.
//
// **This is the maths under `IRegistrationEngine::Refine`, and it is here rather than in that engine
// because it needs no pixels.** `EstimatePairwise` needs OpenCV; this needs quaternions. Keeping
// them apart is what lets a build without OpenCV — every browser build today (ADR 0052) — have the
// half of registration that is arithmetic, the day something wires it up.
//
// **The problem it solves is that a chain has no memory.** Eleven pairwise rotations chained in
// order give twelve absolute rotations, and every error in step k is carried by every frame after
// it: the last frame holds the accumulated error of eleven independent estimates. A ring closes, so
// the twelfth edge — the last frame back to the first — is a measurement the chain throws away, and
// it is exactly the one that says how much drift accumulated. Averaging uses every edge at once and
// spreads the disagreement instead of piling it on the end.
//
// **The gauge comes from the anchors and nothing else.** Relative rotations determine the answer
// only up to one common rotation applied to every frame — turn the whole reconstruction and it is
// the same reconstruction — so a solver given edges alone cannot say which way is north. The anchors
// are what fix it, and they are weighted low on purpose: a fused phone orientation is out by degrees
// where a registered pair is out by hundredths, so the anchors make the answer absolute and the
// edges make it right.

// A measured relation between two frames, in the convention `IRegistrationEngine` answers in.
//
// `rotation` is `conjugate(q[to]) * q[from]`, so `q[from] = q[to] * rotation` and
// `q[to] = q[from] * conjugate(rotation)`. That is what `PairwiseResult::relativeRotation` carries
// for `EstimatePairwise(a, b, ...)` with `from = a` and `to = b`, derived in the `Chain` docblock of
// `core/test/engines/registration_accuracy_test.cpp` rather than discovered by trying both.
struct RelativeRotation {
  int32_t from = 0;
  int32_t to = 0;
  Quat rotation;

  // How much this edge is believed, relative to the others and to the anchors. Evidence rather than
  // a distribution: raw inlier counts are a fine thing to pass. Zero removes the edge from the solve
  // without removing it from the input, which is what a caller holding a parallel array of
  // `PairwiseResult` wants for an unaccepted one (ADR 0056: an unaccepted result still carries the
  // best rotation the pixels offered, and a caller may decide to weigh it at nothing).
  double weight = 1.0;
};

struct AveragedRotations {
  // Parallel to `anchors` **on a valid answer**, and empty on a refusal — the qualification matters
  // because a caller reading the field name rather than `valid` would index an empty vector. A frame
  // that could not be placed holds the identity and is named in
  // `unplaced` — never a silent identity, because the identity is a perfectly ordinary rotation that
  // a phone held level reports, and a caller has no other way to tell one that was solved for from
  // one that was given up on. An unplaced frame is always one with no usable anchor: a frame that
  // has one is placed by it before any edge is walked.
  std::vector<Quat> rotations;

  // Frames no chain of believed edges connects to an anchor, ascending. Their `rotations` entry is
  // the identity, which is why they have to be named: the identity is a rotation a level phone
  // reports, so nothing about the value says it was given up on. With `edgesUsed` and `ambiguous`,
  // this is how much of the answer rests on nothing.
  //
  // **Connectivity, not incidence**, which is a correction: this said "no believed edge reached" and
  // the two differ on the case that matters. A whole component with no anchor in it is unplaced
  // however well its frames are measured against each other — there is nothing to place it
  // *relative to*, since the gauge inside it is free — so two frames joined by a perfect edge and
  // anchored by neither are both named here, and `edgesUsed` does not count the edge between them.
  // `TheEdgeErrorCarriesTheCountItWasComputedOver` builds exactly that. Reachable in life through
  // ADR 0056: a run of unaccepted pairs weighed at zero severs an arc from the rest.
  std::vector<int32_t> unplaced;

  // How far the answer leaves each edge it used, in degrees. **Read them with `edgesUsed`**, which is
  // the whole reason that field exists: zero disagreement over eleven edges and zero disagreement
  // over none are the same pair of numbers and are not the same fact, and the second is what a
  // reconstruction built from priors alone reports. A caller testing `valid && maxEdgeErrorDeg < x`
  // accepts a sphere no pixel contributed to.
  //
  // Two kinds of edge are left out. An edge at weight zero is not used — the caller discarded it.
  // Neither is an edge whose endpoints were never placed, which for a positive-weight edge means
  // *both* of them, since an edge is what puts two frames in one component.
  double medianEdgeErrorDeg = 0;
  double maxEdgeErrorDeg = 0;

  // How many edges the two figures above are computed over, and the denominator of any judgement
  // made from them.
  int32_t edgesUsed = 0;

  // How many anchors were usable rotations — the priors' counterpart to `edgesUsed`, and the only
  // field that separates a reconstruction the sensors agreed on from one a single surviving prior
  // pinned. **The degraded case reports better than the healthy one on everything else**, which is
  // why it needs its own number: one anchor plus a spanning tree is an exact fixed point, so the
  // solve settles in one sweep with both edge errors at zero, while twelve mutually inconsistent
  // priors take hundreds of sweeps and leave a residual.
  int32_t anchorsUsed = 0;

  // Frames no believed edge touches, ascending. They are placed — by their own anchor — so they are
  // not in `unplaced`, and they rest on no pixel at all.
  //
  // `edgesUsed` cannot say this, because it counts edges and the hazard is measured in frames:
  // eleven edges among half a sphere reads exactly like eleven edges across all of it. Reachable
  // without anything going wrong, since ADR 0056 leaves a caller weighing an unaccepted pair at zero
  // and a run of those can strand a whole arc on its priors.
  std::vector<int32_t> priorOnly;

  // Frames whose average had no single maximiser **on any sweep**, so where they sit was settled at
  // some point by the eigensolver's scan order rather than by the evidence. Ascending, each frame
  // named once. Reachable when everything speaking for a frame disagrees by a half turn — two
  // neighbours pointing opposite ways, which in a capture means a registration that has gone badly
  // wrong rather than one that is merely imprecise.
  //
  // **Any sweep, not the last.** A frame placed by a coin flip inherits that placement through
  // every sweep that follows, so a later iteration finding a single maximiser does not make the
  // answer evidence-based. The question a caller is asking is whether the answer rests on an
  // arbitrary choice, not whether the final iteration re-rolled it.
  //
  // Disjoint from `unplaced` — an unplaced frame is never averaged — so the two together say how
  // much of the answer rests on something.
  std::vector<int32_t> ambiguous;

  // How many sweeps were run. At least one on any `valid` answer, so zero means the whole solve was
  // refused — which `valid` already says, and this does not independently promise.
  int32_t sweeps = 0;

  // False when the sweep budget ran out before the largest per-frame update fell under tolerance.
  // The rotations are still the best the solver reached; what is not promised is that another sweep
  // would leave them alone.
  bool converged = false;

  bool valid = false;
};

// Solve for absolute rotations, given relative measurements between frames and per-frame priors.
//
// `anchors` is one entry per frame, indexed by the same `int32_t` the edges name, and `anchorWeight`
// is what every usable anchor counts for against the edges' own weights. A frame whose anchor is not
// a usable rotation (see `IsUsableRotation`) simply has no prior — which is how a caller says "the
// sensor did not answer for this one" without having to renumber the frames.
//
// **That makes an unusable anchor a silence and an unusable edge a refusal, which is an asymmetry
// worth the sentence it costs.** A missing prior is an ordinary thing: a sensor drops a sample, a
// frame is adopted from a resumed session, and the rest of the reconstruction is still an answer.
// A missing *edge* rotation is not ordinary — an edge exists because something measured it, so a
// quaternion that is not one means the measurement is broken rather than absent, and there is no
// "this edge is present but says nothing" for it to mean.
//
// The cost is that a NaN arriving from upstream arithmetic is read as "no prior" rather than as the
// defect it is, and nothing here can tell the two apart. What bounds it is `unplaced` and
// `edgesUsed`: a caller whose priors have all quietly become NaN gets a gauge pinned by whichever
// frames survived, and both fields say how little the answer rests on.
//
// An `anchorWeight` of zero leaves the anchors doing exactly one job: they are where the frames
// start, which is what fixes the gauge, and they are not consulted again. That is a caller saying
// "start from the priors and then believe only the pixels", and it is still one usable anchor away
// from a refusal.
//
// **The whole answer is refused rather than partly given** when the input cannot be read as a
// problem: no frames, an edge naming a frame that is not there, an edge from a frame to itself, an
// edge whose rotation is not one, an edge weight that is negative or not finite, an `anchorWeight`
// that is negative or not finite, or no usable anchor at all — with no anchor the gauge is free and
// every answer is as good as every other, so there is nothing to return that a caller could act on.
//
// An index is checked whatever the weight beside it: the weight says how much a *measurement* is
// believed, and an index naming no frame is not a measurement to disbelieve at a low weight.
//
// A frame with no usable anchor that no surviving edge connects back to an anchored one is a
// different case and is not a refusal: the rest of the reconstruction is still an answer. It is
// named in `unplaced`. "Connects back to", rather than "reaches", for the reason that field's own
// docblock now gives at length — an unanchored component is unplaced however well measured.
//
// **The order of `edges` can change the answer, and only on input that is already contradictory.**
// The breadth-first placement walks edges in the order it is given them, so on a graph whose edges
// disagree about where a frame belongs, which one places it first decides which basin the sweep then
// relaxes into. Measured: three mutually contradictory edges reordered give frames at `0/120/0`
// versus `180/60/0`, both converged. On well-conditioned input it does not arise — 6,426 trials with
// consistent edges jittered half a degree and priors three degrees out produced zero divergences.
//
// Left as it is rather than made order-independent, because the case it affects is one the answer
// already reports as bad: `maxEdgeErrorDeg` is 180 degrees in both of those orderings, so a caller
// reading the number this struct exists to give them is told the reconstruction is worthless either
// way. Making the traversal canonical would buy a *reproducible* worthless answer.
AveragedRotations AverageRotations(std::span<const RelativeRotation> edges,
                                   std::span<const Quat> anchors, double anchorWeight);

}  // namespace sphanorama
