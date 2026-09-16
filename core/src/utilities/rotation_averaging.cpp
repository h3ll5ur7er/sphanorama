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
// with `dot` one ulp from 1 — so its smallest non-zero output is about 8.5e-7 degrees, and it
// answers in quantised steps of that above it. The first version of this constant was 1e-9, which
// is *below* that floor: it could only ever be met by a move of exactly zero, so a solve that had
// genuinely stopped reported `converged == false` and burned its whole budget. Measured: at 1e-9 the
// per-sweep move plateaus at a fixed 1.7e-6 or 3.0e-6 and stays there for thousands of sweeps.
//
// 1e-5 degrees is an order of magnitude above that floor and is 0.036 arcseconds — four orders below
// the hundredths of a degree the registration figures are quoted in, so nothing this decides is
// visible to anything downstream.
constexpr double kSettledDeg = 1e-5;

// How many sweeps the solver is allowed before it reports that it did not settle.
//
// **Measured, and what it is measured against is `anchorWeight` rather than the frame count.** Sweeps
// to settle, over a ring with anchors three degrees out, a dash where 20,000 was not enough:
//
//     frames   1e-4     1e-3     0.01     0.03      0.1        1       10
//          2  17283     2890      407      156       55       10        4
//          3      -     5090      741      286      101       16        5
//         12  12498     3560      590      235       86       16        6
//         60    650     2184      453      198       79       16        6
//         90    894     1328      399      184       74       15        6
//
// It scales as roughly 1/anchorWeight and hardly with size at all, which is not the shape a reader
// expects and is worth knowing: what is still moving in the slow cases is the **gauge** — one common
// rotation shared by every frame — and a weak anchor is exactly a weak constraint on the gauge. The
// relative structure is settled long before.
//
// 1000 covers every size from 2 to 90 at `anchorWeight` 0.01 and above, where the worst is 741. It
// does not cover 1e-3 and below, and deliberately: that regime wants thousands of sweeps to pin a
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
  if (anchors.empty()) return out;
  if (!std::isfinite(anchorWeight) || anchorWeight < 0.0) return out;

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
  // **Without one anchor the gauge is free**, and a free gauge is not a hard problem — it is a
  // problem with no single answer. Every reconstruction turned bodily is the same reconstruction, so
  // what would come back is whatever this function's initialisation happened to start from, which is
  // an arbitrary choice with `valid` true on it.
  if (!anyAnchor) return out;

  std::vector<std::vector<Incidence>> incident(static_cast<size_t>(frames));
  for (size_t k = 0; k < edges.size(); ++k) {
    // A weight of zero removes the edge from the solve without removing it from the caller's array
    // (ADR 0056). Dropped here rather than skipped at every use, so there is one place that decides
    // it and the reported edge error below agrees with the solve by construction.
    if (!(edges[k].weight > 0.0)) continue;
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
      for (const Incidence& touch : incident[static_cast<size_t>(i)]) {
        const RelativeRotation& edge = edges[static_cast<size_t>(touch.edge)];
        const int32_t other = touch.asTo ? edge.from : edge.to;
        if (placed[static_cast<size_t>(other)] == 0) continue;
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

  // How far the answer leaves each edge it used. Over the edges that were used, so a dropped one is
  // not silently counted as satisfied — an edge at weight zero is not evidence about the answer in
  // either direction.
  std::vector<double> errors;
  errors.reserve(edges.size());
  for (const RelativeRotation& edge : edges) {
    if (!(edge.weight > 0.0)) continue;
    if (placed[static_cast<size_t>(edge.from)] == 0) continue;
    if (placed[static_cast<size_t>(edge.to)] == 0) continue;
    const Quat measured = Normalize(Multiply(Conjugate(solved[static_cast<size_t>(edge.to)]),
                                             solved[static_cast<size_t>(edge.from)]));
    errors.push_back(AngleBetween(measured, edge.rotation) * kDegPerRad);
  }
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
