#include "utilities/rotation_averaging.h"

#include <algorithm>
#include <cmath>
#include <limits>
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
// is **5.9 times** the floor rather than the order of magnitude claimed here before.
//
// **And 5.9 times is close enough to the floor that the next decade down is not available**, which
// is the measurement that pins this constant from below for the first time. At 1e-6 the largest
// move can only fall under the threshold by being *exactly* zero, since the smallest non-zero answer
// is 1.7e-6 — so an arrangement whose moves settle into a quantisation cycle rather than onto zero
// never converges at all. Measured, with a budget of 200,000 and three-degree anchors:
//
//     frames / anchorWeight      1e-5        1e-6
//               12 / 0.01     590         768
//               60 / 1.00      16          34
//               90 / 10.0       6      never settles
//
// The last row is the argument: ninety frames at an anchor weight of ten is the *easy* case, six
// sweeps at the committed tolerance, and tightening by one decade turns it into a solve that runs
// forever. So this number is bracketed — 1.7e-6 below it and a regime that stops converging just
// past that — rather than chosen for roundness.
//
// What it costs is that **the answer is where the solver stopped, not the fixed point it was heading
// for**: run the closing-edge fixture to convergence and its worst frame is 2.4e-6 degrees rather
// than the 2.8e-5 this tolerance returns. That is a fact about this constant and is written wherever
// those figures are, because a reader who takes 2.8e-5 for the solver's precision has the cause
// wrong by a factor of eleven.
constexpr double kSettledDeg = 1e-5;

// How many sweeps the solver is allowed before it reports that it did not settle.
//
// **Measured, and what it is measured against is `anchorWeight` rather than the frame count.** Sweeps
// to settle, over a ring with anchors three degrees out:
//
//     frames   1e-4     1e-3     0.01     0.03      0.1        1       10
//          2  27676     5072      744      287      100       16        5
//          3  27610     5090      741      286      101       16        5
//         12  12498     3560      590      235       86       16        6
//         60    650     2184      453      198       79       16        6
//         90    894     1328      399      184       74       15        6
//
// **Every row is a closed ring, including the first**, which is worth saying because a two-frame
// ring's closing edge is a second copy of its only edge and behaves quite differently from a single
// one: measured on one edge that row reads 17283 and 407 rather than 27676 and 744. The 1e-4 column
// needs a budget of 60,000 to finish at all, so a run under 20,000 shows dashes at three and four
// frames that are the budget rather than a failure to converge.
//
// From 1e-3 rightward it scales as roughly 1/anchorWeight and hardly with size at all, which is not
// the shape a reader expects and is worth knowing: what is still moving in the slow cases is the
// **gauge** — one common rotation shared by every frame — and a weak anchor is exactly a weak
// constraint on the gauge. The relative structure is settled long before.
//
// **The 1e-4 column is the exception and is not read that way.** It runs from 650 to 27,676 across
// frame counts, so at that weight the size matters again. It is left in the table rather than
// trimmed out of it, because a table that only showed the regime the claim holds in would be
// evidence for a claim nobody could check.

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

  // No separate empty check: with no anchors there is no usable anchor either, so the gate further
  // down refuses the same input for a reason that is actually about the problem. Nothing between
  // here and there indexes `anchors` or needs `frames` positive.

  // **`anchors` is what says how many frames there are.** There is no separate count for an edge to
  // be checked against, which is deliberate: two numbers meaning "how many frames" is two numbers
  // that can disagree, and the disagreement would be invisible until an index went past one of them.
  //
  // Refused above `INT32_MAX` rather than cast, because the cast is where this stops being arithmetic
  // and starts being undefined. A reviewer reproduced both ends of it on a `MAP_NORESERVE` mapping,
  // which hands out a span of any length for no physical pages: at 2^31 frames the cast wraps to
  // `INT32_MIN`, the `anchored` vector asks for 1.8e19 bytes and throws — and the core is built
  // `-fno-exceptions`, so that is a `std::terminate` rather than a refusal. At 2^32 + 3 it wraps to
  // 3 and answers `valid == true` over a silently truncated three-frame solve, with every edge naming
  // a frame that is right there refused as "naming a frame that is not there".
  //
  // Not reachable from any caller in this tree, and wasm32 cannot reach it at all — `size_t` is 32
  // bits there and this would need 64 GiB inside a 4 GiB space. It is one line, and the header
  // already promises `int32_t` indexing, so the promise may as well be the one that is enforced.
  if (anchors.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) return out;
  if (edges.size() > static_cast<size_t>(std::numeric_limits<int32_t>::max())) return out;
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
    if (anchored[static_cast<size_t>(i)] != 0) ++out.anchorsUsed;
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
  // come to mean two things.
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
  // **Accumulated across sweeps rather than cleared at the top of each**, and that is a correction
  // rather than the first instinct. This cleared, on the reading that what comes back should describe
  // the answer being returned; the comment beside it claimed the difference was untestable and that
  // a transient ambiguity "has not been constructible". A reviewer constructed one in three lines —
  // three frames, two believed edges a half turn apart, the middle frame unanchored — and it settles
  // in three sweeps with the middle frame at the identity, exactly between two neighbours pointing
  // opposite ways. Cleared, it came back unnamed.
  //
  // That input is what settles which predicate is right, and it is not the one the field started
  // with. A frame whose placement was decided by the eigensolver's scan order on *any* sweep is
  // sitting where an arbitrary choice put it, and a later sweep finding a unique maximiser does not
  // undo the choice — it inherits it. "Was this frame ever placed by a coin flip" is the question a
  // caller is asking; "did the last iteration happen to re-roll it" is not.
  std::vector<Quat> predictions;
  std::vector<double> weights;
  std::vector<char> everAmbiguous(static_cast<size_t>(frames), 0);
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
      // No `placed` test on the neighbour, and one is not needed: a believed edge is what puts two
      // frames in one component, so `i` being placed means every frame a believed edge reaches from
      // it is placed too. The guard above does the work for both, and does it by skipping the whole
      // loop rather than each turn of it.
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
      if (!average.isUnique) everAmbiguous[static_cast<size_t>(i)] = 1;

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
  // One `placed` test rather than two, and the missing one is deliberate: a believed edge is exactly
  // what puts its two frames in one component, so its endpoints are placed together or not at all.
  // Checking `to` as well is a guard that cannot fire, over every graph on two to four frames.
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

  // Built from the flags at the end rather than appended to as they are set, which is what makes both
  // of the header's promises — each frame once, ascending — properties of this loop rather than of
  // the order sweeps happened to touch things in. Appending gave neither: a frame first flagged on a
  // later sweep landed after one flagged earlier with a lower index, and nothing deduplicated.
  //
  // The loop is covered rather than self-evident, which is a correction to what stood here. Skipping
  // index 0, or running the range backwards, is a one-token change that leaves both promises false —
  // so the fixtures below it include an ambiguous frame **at index zero** and a case with **two**
  // ambiguous frames, neither of which the branch had while every fixture put its unanchored frame
  // at index 1.
  for (int32_t i = 0; i < frames; ++i) {
    if (everAmbiguous[static_cast<size_t>(i)] != 0) out.ambiguous.push_back(i);
  }

  // Frames no believed edge reaches, which `incident` already knows — it holds only believed edges,
  // so an empty entry is exactly "nothing measured touches this frame". Placed ones only: an
  // unplaced frame rests on nothing at all and is named in `unplaced` instead.
  for (int32_t i = 0; i < frames; ++i) {
    if (placed[static_cast<size_t>(i)] == 0) continue;
    if (incident[static_cast<size_t>(i)].empty()) out.priorOnly.push_back(i);
  }
  out.rotations = std::move(solved);
  out.valid = true;
  return out;
}

}  // namespace sphanorama
