#include "utilities/rotation_averaging.h"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "utilities/quaternion.h"
#include "utilities/quaternion_average.h"

namespace sphanorama {
namespace {

constexpr double kDegPerRad = 180.0 / std::numbers::pi;

// The largest per-frame move, in degrees, below which a sweep counts as having changed nothing.
//
// **Bounded below by the resolution of the thing that measures it**, which is the interesting half.
// The move is `AngleBetween`, and `AngleBetween` of two nearly-equal rotations is `2 acos(|dot|)`
// with `dot` one ulp from 1 — so its smallest non-zero output is **1.7075e-6 degrees**, and above
// that it answers in quantised steps: 1.7075e-6, 2.4148e-6, 2.9576e-6 for the first three ulps.
//
// The first version of this constant was 1e-9, which is *below* that floor: it could only ever be
// met by a move of exactly zero, so a solve that had genuinely stopped reported `converged == false`
// and burned its whole budget, with the per-sweep move parked on a fixed 1.7e-6 or 3.0e-6 for
// thousands of sweeps. Those two plateaus are the first and third steps above, which is the evidence
// that the floor is what they are.
//
// A reviewer caught the floor itself stated at half its value — 8.5e-7 is `acos`, and `AngleBetween`
// doubles it — in the same paragraph whose own measurements are the doubled figures. So 1e-5 degrees
// is **5.9 times** the floor rather than the order of magnitude claimed here before. That is still
// 0.036 arcseconds, four orders below the hundredths of a degree registration is quoted in, so the
// constant does not move; what was wrong was the margin it was described as having.
constexpr double kSettledDeg = 1e-5;

// How many sweeps the solver is allowed before it reports that it did not settle.
//
// **Measured, and what it is measured against is `anchorWeight` rather than the frame count.** Sweeps
// to settle, over a ring with anchors three degrees out, a dash where 20,000 was not enough:
//
//     frames   1e-4     1e-3     0.01     0.03      0.1        1       10
//          2  27676     5072      744      287      100       16        5
//          3  27610     5090      741      286      101       16        5
//         12  12498     3560      590      235       86       16        6
//         60    650     2184      453      198       79       16        6
//         90    894     1328      399      184       74       15        6
//
// Every row is a closed ring, including the first — and that is a correction rather than a detail. A
// reviewer found the two-frame row measured on a *single* edge while every other row had the closing
// one, which is the topology this whole file exists to be about; re-measured as a ring it reads 27676
// rather than 17283 and 744 rather than 407. The 1e-4 column also needed a budget of 60,000 to finish
// at all, so the dashes an earlier version of this table carried at three and four frames were the
// 20,000 it was measured under, not a failure to converge.
//
// From 1e-3 rightward it scales as roughly 1/anchorWeight and hardly with size at all, which is not
// the shape a reader expects and is worth knowing: what is still moving in the slow cases is the
// **gauge** — one common rotation shared by every frame — and a weak anchor is exactly a weak
// constraint on the gauge. The relative structure is settled long before.
//
// **The 1e-4 column is the exception and is not read that way.** It runs from 650 to 17,283 across
// frame counts and refuses to settle at three and four, so at that weight the size matters again. It
// is left in the table rather than trimmed out of it, because a table that only showed the regime
// the claim holds in would be evidence for a claim nobody could check.
//
// 1000 covers every size from 2 to 90 at `anchorWeight` 0.01 and above, where the worst is **744**.
// It does not cover 1e-3 and below, and deliberately: that regime wants thousands of sweeps to pin a
// rotation nothing downstream can see, and `converged` is reported rather than promised for exactly
// this reason.
constexpr int kMaxSweeps = 1000;

// One end of one edge, from the point of view of a frame it touches.
struct Incidence {
  int32_t edge = 0;
  bool asTo = false;   // true when this frame is the edge's `to`
};

}  // namespace

AveragedRotations AverageRotations(std::span<const RelativeRotation> edges,
                                   std::span<const Quat> anchors, double anchorWeight) {
  AveragedRotations out;
  if (!std::isfinite(anchorWeight) || anchorWeight < 0.0) return out;

  // No separate empty check, and a reviewer had to point out why one is not needed: with no anchors
  // there is no usable anchor either, so the gate further down refuses the same input for a reason
  // that is actually about the problem. The test named for the empty case was reaching that gate
  // rather than an empty check all along.

  // **`anchors` is what says how many frames there are.** There is no separate count for an edge to
  // be checked against, which is deliberate: two numbers meaning "how many frames" is two numbers
  // that can disagree, and the disagreement would be invisible until an index went past one of them.
  const int32_t frames = static_cast<int32_t>(anchors.size());

  // The whole input is checked before any of it is used, so a refusal is decided by the input rather
  // than by how far a loop got. An index is checked whatever its weight: the weight says how much a
  // *measurement* is believed, and an index naming no frame is not a measurement to disbelieve.
  for (const RelativeRotation& edge : edges) {
    if (edge.from < 0 || edge.from >= frames || edge.to < 0 || edge.to >= frames) return out;
    if (edge.from == edge.to) return out;
    if (!IsUsableRotation(edge.rotation)) return out;
    if (!std::isfinite(edge.weight) || edge.weight < 0.0) return out;
  }

  std::vector<char> anchored(static_cast<size_t>(frames), 0);
  bool anyAnchor = false;
  for (int32_t i = 0; i < frames; ++i) {
    anchored[static_cast<size_t>(i)] = IsUsableRotation(anchors[static_cast<size_t>(i)]) ? 1 : 0;
    anyAnchor = anyAnchor || anchored[static_cast<size_t>(i)] != 0;
  }
  // **Without one usable anchor there is nothing to start from**, which is a stronger fact than the
  // gauge being free and is the one that decides this. The walk below seeds from anchored frames and
  // spreads outward along the edges, so with none of them anchored nothing is ever placed: every
  // frame would come back holding the identity, named in `unplaced`, and `valid` would be true over
  // an answer with no content.
  //
  // Stated this way because the reason written here first — "an arbitrary gauge with `valid` true on
  // it" — is word for word the case this function *accepts* two paragraphs later, where
  // `anchorWeight` is zero and the anchors place the frames without being believed. A reviewer put
  // the two side by side. The difference is not how much the anchors are trusted; it is whether
  // there is a frame to begin at.
  if (!anyAnchor) return out;

  // A weight of zero removes the edge from the solve without removing it from the caller's array
  // (ADR 0056). **Decided once, here, and recorded** — the error report below reads `believed` rather
  // than re-testing the weight, because a predicate written in two places is a predicate that can
  // come to mean two things, and a reviewer found this comment already claiming the single decision
  // it did not yet have.
  std::vector<std::vector<Incidence>> incident(static_cast<size_t>(frames));
  std::vector<char> believed(edges.size(), 0);
  for (size_t k = 0; k < edges.size(); ++k) {
    if (!(edges[k].weight > 0.0)) continue;
    believed[k] = 1;
    incident[static_cast<size_t>(edges[k].from)].push_back(Incidence{static_cast<int32_t>(k), false});
    incident[static_cast<size_t>(edges[k].to)].push_back(Incidence{static_cast<int32_t>(k), true});
  }

  // **A starting point every frame can be improved from**, which the sweep below cannot produce for
  // itself: a frame whose neighbours are all unplaced has nothing to average, so without this the
  // solve would never leave the anchored frames. Anchored frames start at their anchor and the rest
  // are walked outward along the edges, which is the chain this whole file exists to improve on —
  // used here for what a chain is good for, namely getting everything into roughly the right place.
  std::vector<Quat> solved(static_cast<size_t>(frames), Quat{});
  std::vector<char> placed(static_cast<size_t>(frames), 0);
  std::vector<int32_t> reached;
  reached.reserve(static_cast<size_t>(frames));
  for (int32_t i = 0; i < frames; ++i) {
    if (anchored[static_cast<size_t>(i)] == 0) continue;
    solved[static_cast<size_t>(i)] = Normalize(anchors[static_cast<size_t>(i)]);
    placed[static_cast<size_t>(i)] = 1;
    reached.push_back(i);
  }
  for (size_t head = 0; head < reached.size(); ++head) {
    const int32_t at = reached[head];
    for (const Incidence& touch : incident[static_cast<size_t>(at)]) {
      const RelativeRotation& edge = edges[static_cast<size_t>(touch.edge)];
      const int32_t other = touch.asTo ? edge.from : edge.to;
      if (placed[static_cast<size_t>(other)] != 0) continue;
      // `rotation` is `conjugate(q[to]) * q[from]`, so `q[from] = q[to] * rotation` and
      // `q[to] = q[from] * conjugate(rotation)`.
      solved[static_cast<size_t>(other)] =
          touch.asTo ? Normalize(Multiply(solved[static_cast<size_t>(at)], edge.rotation))
                     : Normalize(Multiply(solved[static_cast<size_t>(at)],
                                          Conjugate(edge.rotation)));
      placed[static_cast<size_t>(other)] = 1;
      reached.push_back(other);
    }
  }
  for (int32_t i = 0; i < frames; ++i) {
    if (placed[static_cast<size_t>(i)] == 0) out.unplaced.push_back(i);
  }

  // **Each frame becomes the weighted average of what everything touching it says it should be.**
  // Gauss-Seidel rather than a batch update: a frame moved this sweep is read by its neighbours
  // later in the same sweep, so information crosses the graph in one pass instead of one hop per
  // pass — which on a ring is the difference between converging in a handful of sweeps and in as
  // many as there are frames.
  std::vector<Quat> predictions;
  std::vector<double> weights;
  for (int sweep = 1; sweep <= kMaxSweeps; ++sweep) {
    double largestMoveDeg = 0;
    for (int32_t i = 0; i < frames; ++i) {
      if (placed[static_cast<size_t>(i)] == 0) continue;
      predictions.clear();
      weights.clear();

      // An `anchorWeight` of zero leaves the anchors doing exactly one job: they placed the frames
      // above, which is what fixes the gauge, and they are not consulted again. That is a caller
      // saying "start from the priors and then believe only the pixels", and it is a weight of zero
      // meaning what it means everywhere else here.
      if (anchored[static_cast<size_t>(i)] != 0 && anchorWeight > 0.0) {
        predictions.push_back(Normalize(anchors[static_cast<size_t>(i)]));
        weights.push_back(anchorWeight);
      }
      // No `placed` test on the neighbour, and it was there until a reviewer showed that neither of
      // the two guards could be removed *alone* without the other hiding it. A believed edge is what
      // puts two frames in one component, so `i` being placed means every frame a believed edge
      // reaches from it is placed too. One of the pair is load-bearing and the other is decoration;
      // the one kept is the cheaper, since it skips the whole loop rather than each turn of it.
      for (const Incidence& touch : incident[static_cast<size_t>(i)]) {
        const RelativeRotation& edge = edges[static_cast<size_t>(touch.edge)];
        const int32_t other = touch.asTo ? edge.from : edge.to;
        predictions.push_back(
            touch.asTo
                ? Normalize(Multiply(solved[static_cast<size_t>(other)], Conjugate(edge.rotation)))
                : Normalize(Multiply(solved[static_cast<size_t>(other)], edge.rotation)));
        weights.push_back(edge.weight);
      }
      if (predictions.empty()) continue;

      // **No check on the answer, because this call cannot refuse.** `AverageQuaternions` turns down
      // an empty set, a mismatched weight span, an input that is not a rotation, a negative or
      // non-finite weight, or weights that are all zero. None is reachable from here: the empty case
      // returns above, the spans are filled together, every prediction is a `Normalize` of a product
      // of rotations and so is one, and every weight pushed is either `anchorWeight` past its
      // `> 0.0` test or an edge weight past the filter that built `incident`. A guard here would read
      // as protection to the next person and could never fire, which this repository treats as worse
      // than none.
      const QuaternionAverage average = AverageQuaternions(predictions, weights);
      largestMoveDeg = std::max(
          largestMoveDeg,
          AngleBetween(solved[static_cast<size_t>(i)], average.rotation) * kDegPerRad);
      solved[static_cast<size_t>(i)] = average.rotation;
    }

    out.sweeps = sweep;
    if (largestMoveDeg < kSettledDeg) {
      out.converged = true;
      break;
    }
  }

  // How far the answer leaves each edge it used. A dropped edge is not silently counted as satisfied
  // — it is not evidence about the answer in either direction — and `edgesUsed` is what lets a caller
  // tell "nothing disagrees" from "nothing was consulted".
  //
  // One `placed` test rather than two, and the missing one is deliberate. A believed edge is exactly
  // what puts its two frames in one component, so its endpoints are placed together or not at all;
  // checking `to` as well was a guard a reviewer showed could not fire, over an exhaustive search of
  // every graph on two to four frames.
  std::vector<double> errors;
  errors.reserve(edges.size());
  for (size_t k = 0; k < edges.size(); ++k) {
    if (believed[k] == 0) continue;
    const RelativeRotation& edge = edges[k];
    if (placed[static_cast<size_t>(edge.from)] == 0) continue;
    const Quat measured = Normalize(Multiply(Conjugate(solved[static_cast<size_t>(edge.to)]),
                                             solved[static_cast<size_t>(edge.from)]));
    errors.push_back(AngleBetween(measured, edge.rotation) * kDegPerRad);
  }
  out.edgesUsed = static_cast<int32_t>(errors.size());
  if (!errors.empty()) {
    std::sort(errors.begin(), errors.end());
    const size_t half = errors.size() / 2;
    out.medianEdgeErrorDeg =
        (errors.size() % 2 == 1) ? errors[half] : (errors[half - 1] + errors[half]) * 0.5;
    out.maxEdgeErrorDeg = errors.back();
  }

  out.rotations = std::move(solved);
  out.valid = true;
  return out;
}

}  // namespace sphanorama
