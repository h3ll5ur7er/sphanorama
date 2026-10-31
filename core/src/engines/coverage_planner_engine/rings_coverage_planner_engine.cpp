#include "engines/coverage_planner_engine/rings_coverage_planner_engine.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <iterator>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "engines/coverage_planner_engine/guidance_roll.h"
#include "utilities/camera_model.h"
#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr const char* kComponent = "RingsCoveragePlannerEngine";
constexpr double kRadToDeg = 180.0 / std::numbers::pi;
constexpr double kDegToRad = std::numbers::pi / 180.0;

// Below this the 1/cos(elevation) widening runs away. The pole itself is one cell whatever the
// cosine, so this only reaches the ring beside a pole, and only for a lens of a few degrees.
constexpr double kPolarCosineFloor = 0.08;

// A plan needing more cells than this is not a capture anyone completes: at a two-second dwell it
// is over an hour of holding a phone still. The bound keeps the counts below inside int, since
// converting an out-of-range double to int is undefined behaviour and arrives here as a plan rather
// than as a diagnosable failure. And it is what ends the search in `Plan` in a bounded time: it was
// a hundred thousand, and a cone just short of too wide for its lens laid and checked sixty
// thousand cells for a minute in the browser before answering (ADR 0076).
constexpr double kMaxCells = 2000.0;

// How much smaller than the lens, on every side, the frame `CompleteInTheWorstCase` asks about is.
constexpr double kMarginDeg = 1.0;

// Rings at these steps, or nothing if they would need more cells than a session can hold.
std::optional<std::vector<CoverageNode>> LayRings(const CapturePlanSpec& spec,
                                                  double verticalStep, double horizontalStep) {
  // Rings are laid symmetrically about the horizon and reach the poles, so that a sphere is
  // covered rather than a band. An odd count keeps one ring exactly on the horizon, which is
  // where most of the interesting content is and where the user starts.
  //
  // Every ring holds a cell, so more rings than the bound is refused before any count is made an
  // int; each ring's own count is refused below in the same way, as a double.
  const double halfRingsWanted = std::ceil(90.0 / verticalStep);
  if (!(2.0 * halfRingsWanted + 1.0 <= kMaxCells)) return std::nullopt;

  const int halfRings = static_cast<int>(halfRingsWanted);
  const int ringCount = 2 * halfRings + 1;
  const double elevationStep = 180.0 / (ringCount - 1);

  // Without the caps the plan covers a band around the horizon instead of a sphere. The limit is
  // the highest elevation a cell can aim at and still have its whole field of view below the
  // pole — dropping every ring above that, rather than just the two pole cells, is what makes
  // "no caps" mean a band rather than a sphere with two holes in it.
  const double elevationLimit = spec.coverPoles ? 90.0 : 90.0 - spec.verticalFovDeg / 2.0;

  std::vector<CoverageNode> nodes;
  uint64_t nextId = 1;
  for (int ring = 0; ring < ringCount; ++ring) {
    // Counted from the horizon, so the horizon is exactly 0 and the rings either side of it are
    // exact mirrors. Counted from a pole, the horizon came out a few ulps off, and a band whose
    // limit was the horizon itself lost it.
    const double elevation = elevationStep * (ring - halfRings);
    if (std::abs(elevation) > elevationLimit) continue;

    // A ring's circumference shrinks by cos(elevation), so the azimuth step widens to keep the
    // on-sphere spacing constant. Without this the poles collect hundreds of redundant cells.
    //
    // Measured where the ring's band is widest, which is its edge nearer the horizon, rather than
    // at its centre: the band's frames have to close the circle there too (ADR 0075). A ring at a
    // pole is one cell, which looks at every azimuth at once.
    const bool pole = ring == 0 || ring == ringCount - 1;
    const double widest = std::max(std::abs(elevation) - elevationStep / 2.0, 0.0);
    const double cosine = std::max(std::cos(widest * kDegToRad), kPolarCosineFloor);
    const double cellsWanted =
        pole ? 1.0 : std::max(1.0, std::ceil(360.0 * cosine / horizontalStep));
    // The cells laid, not an estimate of them: a grid of full circles counted half again as many.
    if (!(static_cast<double>(nodes.size()) + cellsWanted <= kMaxCells)) return std::nullopt;
    const int cells = static_cast<int>(cellsWanted);

    for (int cell = 0; cell < cells; ++cell) {
      CoverageNode node;
      node.id = NodeId{nextId++};
      node.targetOrientation = FromAzimuthElevation(360.0 * cell / cells, elevation);
      node.acceptanceConeDeg = spec.acceptanceConeDeg;
      node.ringIndex = ring;
      nodes.push_back(node);
    }
  }
  return nodes;
}

// Whether a direction falls inside the rectangle a camera at `aim` images.
bool Images(const Quat& aim, const Vec3& direction, double halfWidth, double halfHeight) {
  const Vec3 local = Rotate(Conjugate(aim), direction);
  const double depth = -local.z;
  return depth > 0.0 && std::abs(local.x) <= halfWidth * depth &&
         std::abs(local.y) <= halfHeight * depth;
}

// Whether every direction the plan answers for is inside some cell's frame however that frame is
// taken: anywhere on its cone's edge or at its centre, and at a pole at any heading as well, since
// guidance asks for no roll there (ADR 0072). For one direction this is the worst case exactly,
// since each cell's frame is taken once and independently.
//
// Asked at 4000 directions spread over the sphere and 720 on the horizon, with 16 aims around the
// cone and, at a pole, each of those at 12 headings, of a frame a degree smaller than the lens on
// every side. Asked of the frame itself, a denser check found directions between the samples
// that no frame covered; a degree is what a real frame spends covering the directions and aims
// between the ones asked (ADR 0076).
bool CompleteInTheWorstCase(const std::vector<CoverageNode>& nodes, const CapturePlanSpec& spec) {
  std::vector<Quat> aims{Quat{}};
  for (int step = 0; step < 16; ++step) {
    const double around = 2.0 * std::numbers::pi * step / 16.0;
    aims.push_back(FromAxisAngle(Vec3{std::cos(around), std::sin(around), 0.0},
                                 spec.acceptanceConeDeg * kDegToRad));
  }
  std::vector<Quat> atAPole;
  for (int heading = 0; heading < 180; heading += 15) {
    for (const Quat& aim : aims) {
      atAPole.push_back(Multiply(FromAxisAngle(Vec3{0.0, 0.0, 1.0}, heading * kDegToRad), aim));
    }
  }
  double highest = 0.0;
  for (const CoverageNode& node : nodes) {
    highest = std::max(highest, std::abs(Direction(node.targetOrientation).y));
  }
  const double halfWidth = std::tan((spec.horizontalFovDeg * 0.5 - kMarginDeg) * kDegToRad);
  const double halfHeight = std::tan((spec.verticalFovDeg * 0.5 - kMarginDeg) * kDegToRad);

  // A cell can only cover what is within its frame's half-diagonal of where it is aimed, so the
  // rectangles are asked only of cells that near: one cheap product instead of seventeen frames.
  const double reach =
      std::atan(std::hypot(halfWidth, halfHeight)) + spec.acceptanceConeDeg * kDegToRad;
  const double nearEnough = std::cos(reach);
  std::vector<Vec3> aimedAt;
  aimedAt.reserve(nodes.size());
  for (const CoverageNode& node : nodes) aimedAt.push_back(Direction(node.targetOrientation));

  // Spread over the sphere, and the horizon besides: a band left with only its horizon ring answers
  // for nothing else, and no point of the spread lands on it.
  constexpr int kDirections = 4000;
  constexpr int kOnTheHorizon = 720;
  std::vector<Vec3> directions;
  directions.reserve(kDirections + kOnTheHorizon);
  const double golden = std::numbers::pi * (3.0 - std::sqrt(5.0));
  for (int i = 0; i < kDirections; ++i) {
    const double y = 1.0 - 2.0 * (static_cast<double>(i) + 0.5) / kDirections;
    // A band answers only for the elevations its rings aim at; the caps are not its to cover.
    if (std::abs(y) > highest) continue;
    const double radius = std::sqrt(std::max(0.0, 1.0 - y * y));
    directions.push_back(Vec3{std::cos(golden * i) * radius, y, std::sin(golden * i) * radius});
  }
  for (int i = 0; i < kOnTheHorizon; ++i) {
    const double azimuth = 2.0 * std::numbers::pi * (i + 0.5) / kOnTheHorizon;
    directions.push_back(Vec3{std::sin(azimuth), 0.0, -std::cos(azimuth)});
  }

  // Cells are asked nearest first: the ring nearest in elevation, then outward, and in each ring
  // the cell nearest in azimuth, then outward. Which cell covers a direction does not change the
  // answer, only how soon it is found. In the order they were laid, a wide cone left nearly every
  // cell near enough, and a direction walked half the plan before one covered it: seconds in the
  // browser for a plan the bound allows (ADR 0076).
  struct Ring {
    double elevation;
    size_t first;
    size_t count;
  };
  std::vector<Ring> rings;
  for (size_t n = 0; n < nodes.size(); ++n) {
    if (rings.empty() || nodes[n].ringIndex != nodes[rings.back().first].ringIndex) {
      rings.push_back(Ring{std::asin(std::clamp(aimedAt[n].y, -1.0, 1.0)), n, 0});
    }
    ++rings.back().count;
  }
  const auto covers = [&](size_t n, const Vec3& direction) {
    const Vec3& at = aimedAt[n];
    if (at.x * direction.x + at.y * direction.y + at.z * direction.z < nearEnough) return false;
    const std::vector<Quat>& taken = std::abs(at.y) > 1.0 - 1e-9 ? atAPole : aims;
    return std::all_of(taken.begin(), taken.end(), [&](const Quat& aim) {
      return Images(Multiply(nodes[n].targetOrientation, aim), direction, halfWidth, halfHeight);
    });
  };
  const auto coveredInRing = [&](const Ring& ring, const Vec3& direction) {
    const double azimuth = std::atan2(-direction.x, -direction.z) / (2.0 * std::numbers::pi);
    const auto count = static_cast<long>(ring.count);
    const long nearest = std::lround(azimuth * static_cast<double>(count));
    for (long step = 0; step < count; ++step) {
      const long offset = step % 2 == 1 ? (step + 1) / 2 : -(step / 2);
      const long cell = ((nearest + offset) % count + count) % count;
      if (covers(ring.first + static_cast<size_t>(cell), direction)) return true;
    }
    return false;
  };

  for (const Vec3& direction : directions) {
    const double elevation = std::asin(std::clamp(direction.y, -1.0, 1.0));
    auto above = std::lower_bound(rings.begin(), rings.end(), elevation,
                                  [](const Ring& ring, double e) { return ring.elevation < e; });
    auto below = above;
    bool safe = false;
    while (!safe && (below != rings.begin() || above != rings.end())) {
      const bool takeAbove =
          below == rings.begin() ||
          (above != rings.end() &&
           above->elevation - elevation <= elevation - std::prev(below)->elevation);
      safe = coveredInRing(takeAbove ? *above++ : *--below, direction);
    }
    if (!safe) return false;
  }
  return true;
}

}  // namespace

Result<CapturePlan> RingsCoveragePlannerEngine::Plan(const CapturePlanSpec& spec,
                                                     const Intrinsics&) {
  if (spec.strategy != TessellationStrategy::Rings) {
    // Refused rather than substituted. The plan carries the spec back to the caller, so quietly
    // laying rings for a geodesic request would have the plan claim a layout it does not have,
    // and anything reasoning about cell spacing from the strategy it asked for would be reasoning
    // about a different sphere.
    return Err<CapturePlan>(StatusCode::Unsupported, kComponent,
                            "this engine tessellates in rings; no other strategy is implemented");
  }

  // Finiteness first. The spec crosses the facade as doubles, where NaN and Infinity are ordinary
  // values a client can send, and both slip past a ">= 0" test — then reach the conversions
  // below, where a float outside int's range is undefined behaviour rather than a large number.
  if (!std::isfinite(spec.horizontalFovDeg) || !std::isfinite(spec.verticalFovDeg) ||
      !std::isfinite(spec.overlapTarget) || !std::isfinite(spec.acceptanceConeDeg)) {
    return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                            "the plan spec contains a value that is not a finite number");
  }
  if (spec.horizontalFovDeg <= 0.0 || spec.verticalFovDeg <= 0.0) {
    return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                            "the lens field of view is unknown; nothing can be tessellated");
  }
  if (spec.horizontalFovDeg > 180.0 || spec.verticalFovDeg > 180.0) {
    return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                            "a field of view wider than 180 degrees is not a lens");
  }
  if (spec.overlapTarget < 0.0 || spec.overlapTarget >= 1.0) {
    // An overlap of 1 is a step of zero degrees: infinitely many cells.
    return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                            "overlap must be at least 0 and less than 1");
  }
  if (spec.acceptanceConeDeg <= 0.0) {
    return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                            "acceptance cone must be positive");
  }

  // A frame is taken anywhere in its cell's cone (ADR 0075). One aimed at the edge of a cone half
  // as wide as the frame does not see the cell's own centre, and no spacing makes a sphere of
  // those complete; nor, below, can the frame the check asks about, which is a margin smaller.
  if (spec.acceptanceConeDeg + kMarginDeg >=
      std::min(spec.horizontalFovDeg, spec.verticalFovDeg) / 2.0) {
    return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                            "the acceptance cone is too wide for this lens: a frame aimed at its "
                            "edge would not see the cell's own centre");
  }
  const double advance = 1.0 - spec.overlapTarget;
  double verticalStep = spec.verticalFovDeg * advance;
  double horizontalStep = spec.horizontalFovDeg * advance;

  // Checked rather than trusted. Spaced by the overlap alone the plan left holes three ways: the
  // cone ate the overlap on narrow lenses, the caps beside the poles opened for a pole frame turned
  // to its short edge, and the page's own lens was complete by rounding. Each was found by a lens a
  // fraction of a degree from the ones that passed (ADR 0075, ADR 0076). So the plan is laid,
  // asked whether any frame its cones accept could leave a hole, and laid tighter until none
  // could. Rules for each of the three ways were tried first, and the check alone lays about as
  // many cells, fewer in fact. The page's lens is complete as first laid.
  CapturePlan plan;
  plan.spec = spec;
  for (;;) {
    std::optional<std::vector<CoverageNode>> nodes =
        LayRings(spec, verticalStep, horizontalStep);
    if (!nodes) {
      return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                              "this lens, overlap and cone would need more cells than a capture "
                              "could hold");
    }
    if (CompleteInTheWorstCase(*nodes, spec)) {
      plan.nodes = std::move(*nodes);
      break;
    }
    verticalStep *= 0.95;
    horizontalStep *= 0.95;
  }

  return Ok(std::move(plan));
}

Result<CaptureGuidance> RingsCoveragePlannerEngine::Locate(const PoseSample& current,
                                                            const CapturePlan& plan,
                                                            const CoverageState& coverage) {
  if (plan.nodes.empty()) {
    return Err<CaptureGuidance>(StatusCode::FailedPrecondition, kComponent, "plan has no cells");
  }

  // Compared as directions, not as attitudes. AngleBetween on two orientations folds rotation
  // about the optical axis into the answer, so a phone aimed exactly at a cell but held at an
  // angle would read as far off target and the reticle would never close. How the phone is held
  // is a separate correction, and CaptureGuidance has a separate field for it.
  const Vec3 looking = Direction(current.orientation);

  // Whether there is an aim to prefer at all. Zero confidence is the contract's word for "no
  // reading has ever anchored this orientation", which leaves the pose at the identity it was
  // born with — a direction nobody chose. Naming the cell that happens to sit there would put the
  // reticle on it every tick until a reading arrives, and on a stream carrying rates with no
  // attitude in it, forever: the pose would drift off identity without ever being *about*
  // anything, and a dwell keyed on `HoldStill` would mature into a burst at a cell nobody
  // pointed at.
  //
  // This branch used to describe a device — a phone with no motion sensor, which ADR 0042 made a
  // supported configuration and coverage guided alone. That device is refused at `Begin` now
  // (ADR 0044), so what is left here is a session's opening ticks and a stream that anchors
  // nothing: not a way to capture, a wait. Guidance seeks until there is something to be inside
  // of.
  const bool aimed = current.confidence > 0.0;

  // Coverage has an opinion only once something has been evaluated. An empty state is no
  // information rather than nothing missing: at the start of a session nothing is captured and
  // nothing is a hole, and reading that as a finished sphere would end a capture before it began.
  const bool informed = coverage.nodesTotal > 0;
  const auto missing = [&](NodeId id) {
    return std::any_of(coverage.holes.begin(), coverage.holes.end(),
                       [id](NodeId hole) { return hole.value == id.value; });
  };

  const auto nearestOf = [&](bool onlyMissing) -> const CoverageNode* {
    const CoverageNode* found = nullptr;
    double closest = 0.0;
    for (const auto& node : plan.nodes) {
      if (onlyMissing && !missing(node.id)) continue;
      const double angle = AngleBetweenDirections(looking, Direction(node.targetOrientation));
      if (found == nullptr || angle < closest) {
        closest = angle;
        found = &node;
      }
    }
    return found;
  };

  // The cell the camera is *inside*, whatever coverage thinks of it. Aim beats coverage here, and
  // that reverses an earlier rule worth stating rather than quietly dropping.
  //
  // Skipping a captured cell and naming the nearest missing one reads well until the phone stops
  // moving: capture the cell in front of you and the target jumps to a neighbour under a camera
  // that has not turned, so the next press captures a cell nobody is aimed at — and since a burst
  // records whatever the camera sees, it fills that neighbour with this cell's pixels. Three
  // presses at one spot filled three cells, two of them wrong. What the old rule was protecting —
  // never telling someone to re-shoot what they already have — belongs to the *action* below, not
  // to which cell is named.
  const CoverageNode* inside = nullptr;
  double insideAngle = 0.0;
  for (const auto& node : aimed ? std::span<const CoverageNode>(plan.nodes)
                                : std::span<const CoverageNode>()) {
    const double angle = AngleBetweenDirections(looking, Direction(node.targetOrientation));
    // A cone that is not a usable measurement puts no cell inside it. `ArmBurst` used to give the
    // same answer and they had to agree or the reticle closed on a cell that would not arm (see
    // `ICoveragePlannerEngine`'s header); the capture session now refuses such a cell before it
    // keeps a plan (ADR 0077), and this is for a plan handed in. It is spelled to fail closed:
    // `!isfinite` refuses `inf`, where the comparison further down is false and every direction is
    // therefore "inside". Refusing a non-finite cone *here* is also what earns that comparison the
    // right to be a plain `>` rather than a NaN-safe spelling — see its own comment.
    if (!std::isfinite(node.acceptanceConeDeg) || node.acceptanceConeDeg <= 0.0) continue;
    // And the cell has to point somewhere. `AngleBetweenDirections` answers a degenerate direction
    // with `0.0` — "dead on" — which is the same number a camera aimed exactly at the cell
    // produces, so a node whose target is not a rotation is inside any cone from any direction and
    // nothing downstream can tell the two apart. The cone guard above does not cover it: the cone
    // is a perfectly good measurement in that case and the *target* is not.
    if (!IsUsableRotation(node.targetOrientation)) continue;
    // A plain `>`, as `ArmBurst` reads this same number. `!(x <= y)` is the NaN-safe spelling and
    // it is not doing any work here: the two guards above have already refused a non-finite cone
    // and a target that is not a rotation, and `AngleBetweenDirections` cannot answer NaN for
    // usable inputs — so a reviewer swapped it in both files and every test stayed green, which is
    // what a guard that cannot decide anything looks like. Written as the thing it means, so the
    // next reader does not have to work out which NaN it is defending against.
    if (angle * kRadToDeg > node.acceptanceConeDeg) continue;
    if (inside == nullptr || angle < insideAngle) {
      insideAngle = angle;
      inside = &node;
    }
  }

  // Only what is still needed, when that is known. Falling back to the whole plan is not merely
  // defensive: a holes list naming cells this plan does not contain would otherwise leave nothing
  // to aim at, and an odd target beats refusing to guide at all.
  const CoverageNode* stillMissing = informed ? nearestOf(true) : nullptr;
  const bool nothingMissing = informed && stillMissing == nullptr;
  const CoverageNode* nearest = inside != nullptr ? inside : stillMissing;
  if (nearest == nullptr) nearest = nearestOf(false);

  CaptureGuidance guidance;
  // Said out loud, because a client reads it and cannot derive it: the page parks the reticle at
  // its widest and stops correcting for roll while it is false, and neither is a number it can
  // work out from an orientation it never sees.
  guidance.aimKnown = aimed;
  guidance.targetNode = nearest->id;
  // Measured against the cell that was named, rather than carried out of whichever search found
  // it. Two searches ran and only one of them decided.
  guidance.angularErrorDeg =
      AngleBetweenDirections(looking, Direction(nearest->targetOrientation)) * kRadToDeg;
  guidance.rollErrorDeg = GuidanceRollDeg(current.orientation);
  // A finished sphere still names a cell and an error, because the fields are read either way —
  // but it says so, which nothing in this engine ever did before, so a completed capture went on
  // asking for whichever cell the phone happened to be nearest.
  //
  // Inside a cone, the action is the whole difference between a cell worth shooting and one
  // already shot: `HoldStill` asks for a capture, `AlreadyCaptured` says nothing is owed here. A
  // client is free to offer a re-capture on the second — that is what makes one possible at all —
  // but nothing tells the user to make one. An uninformed coverage state means nothing has been
  // evaluated yet, which is not the same as nothing being needed.
  guidance.action =
      nothingMissing ? GuidanceAction::SphereDone
      : inside == nullptr ? GuidanceAction::Seek
      : (!informed || missing(inside->id)) ? GuidanceAction::HoldStill
                                           : GuidanceAction::AlreadyCaptured;
  return Ok(guidance);
}

Result<CoverageState> RingsCoveragePlannerEngine::Evaluate(const CapturePlan& plan,
                                                            std::span<const Candidate> candidates) {
  CoverageState state;
  state.nodesTotal = static_cast<int32_t>(plan.nodes.size());

  for (const auto& node : plan.nodes) {
    const bool covered =
        std::any_of(candidates.begin(), candidates.end(),
                    [&](const Candidate& c) { return c.node.value == node.id.value; });
    if (covered) {
      ++state.nodesSatisfied;
    } else {
      state.holes.push_back(node.id);
    }
  }
  state.coveredSolidAngleFraction =
      state.nodesTotal == 0 ? 0.0
                            : static_cast<double>(state.nodesSatisfied) / state.nodesTotal;
  return Ok(std::move(state));
}

Result<std::vector<NodeId>> RingsCoveragePlannerEngine::SuggestRetakes(const CapturePlan&,
                                                                       const CoverageState& state,
                                                                       const GhostReport& ghosts) {
  // Holes first, then anything ghosted: a cell with no frames at all is a worse problem than one
  // whose frames disagree, and a cell that is both should only be suggested once.
  std::vector<NodeId> suggestions = state.holes;
  for (const auto& region : ghosts.regions) {
    const bool already = std::any_of(suggestions.begin(), suggestions.end(),
                                     [&](NodeId id) { return id.value == region.node.value; });
    if (!already) suggestions.push_back(region.node);
  }
  return Ok(std::move(suggestions));
}

Result<std::vector<CoverageGap>> RingsCoveragePlannerEngine::FindGaps(
    const CapturePlan& plan, std::span<const PlacedFrame> frames, const Intrinsics& fillLens) {
  if (plan.nodes.empty()) {
    return Err<std::vector<CoverageGap>>(
        StatusCode::InvalidArgument, kComponent,
        "a plan with no cells says no directions are its to cover");
  }
  const double cone = plan.spec.acceptanceConeDeg;
  if (!std::isfinite(cone) || cone <= 0.0) {
    return Err<std::vector<CoverageGap>>(StatusCode::InvalidArgument, kComponent,
                                         "a gap is judged against a cone that is a measurement");
  }
  // No point of the sphere is farther than about 1.012 degrees from a direction asked below. A
  // direction is counted covered only where one frame covers it and the ring this far around it,
  // whose hull holds every point that near, so a point left open always has an open direction
  // within 1.012 degrees; and a fill frame reaches this far past its target's directions. Asked of
  // the directions alone, filling left slivers open between them (ADR 0084).
  constexpr double kBetweenDeg = 1.1;
  // What a fill frame covers wherever in the cone it is aimed: the circle inscribed in it, the
  // nearest of its four edges to its axis whatever its centre and distortion, less the cone, less
  // the distance between directions.
  double inscribed = std::numbers::pi / 2.0;
  for (const Pixel& edge : {Pixel{0.0, fillLens.cy}, Pixel{fillLens.width * 1.0, fillLens.cy},
                            Pixel{fillLens.cx, 0.0}, Pixel{fillLens.cx, fillLens.height * 1.0}}) {
    const UnprojectedDirection ray = Unproject(fillLens, edge);
    if (!ray.valid) {
      return Err<std::vector<CoverageGap>>(StatusCode::InvalidArgument, kComponent,
                                           "a fill lens has to see to the edges of its frame");
    }
    inscribed = std::min(inscribed, std::acos(std::clamp(-ray.direction.z, -1.0, 1.0)));
  }
  const double reachDeg = inscribed * kRadToDeg - cone - kBetweenDeg;
  // And no finer than the directions are apart, about a degree and a half: below that every open
  // direction is its own target, tens of thousands, more than a plan may hold.
  constexpr double kSpacingDeg = 1.5;
  if (!(reachDeg >= kSpacingDeg)) {
    return Err<std::vector<CoverageGap>>(
        StatusCode::InvalidArgument, kComponent,
        "the fill lens is too narrow to cover, from inside the cone, more than the directions are "
        "apart");
  }

  // Each frame is asked only of directions within the reach of its corners, where `Project`
  // decides. A corner that does not unproject leaves its whole half-sphere to ask.
  struct Seen {
    Quat rotation;
    Vec3 axis;
    double nearEnough;
    const Intrinsics* lens;
  };
  std::vector<Seen> seen;
  seen.reserve(frames.size());
  for (const PlacedFrame& frame : frames) {
    if (!IsUsableRotation(frame.rotation) || !IsUsableLens(frame.lens)) {
      return Err<std::vector<CoverageGap>>(StatusCode::InvalidArgument, kComponent,
                                           "a placed frame needs a rotation and a lens");
    }
    const Quat rotation = Normalize(frame.rotation);
    double corner = 0.0;
    for (const Pixel& at : {Pixel{0.0, 0.0}, Pixel{frame.lens.width * 1.0, 0.0},
                            Pixel{0.0, frame.lens.height * 1.0},
                            Pixel{frame.lens.width * 1.0, frame.lens.height * 1.0}}) {
      const UnprojectedDirection ray = Unproject(frame.lens, at);
      corner = ray.valid ? std::max(corner, std::acos(std::clamp(-ray.direction.z, -1.0, 1.0)))
                         : std::numbers::pi / 2.0;
    }
    seen.push_back(Seen{rotation, Direction(rotation), std::cos(std::min(corner + 0.01,
                                                                         std::numbers::pi / 2.0)),
                        &frame.lens});
  }
  const auto images = [](const Seen& frame, const Vec3& direction) {
    if (Dot(frame.axis, direction) < frame.nearEnough) return false;
    const ProjectedPixel landing =
        Project(*frame.lens, Rotate(Conjugate(frame.rotation), direction));
    return landing.valid && landing.pixel.x >= 0.0 && landing.pixel.x <= frame.lens->width &&
           landing.pixel.y >= 0.0 && landing.pixel.y <= frame.lens->height;
  };
  const double between = kBetweenDeg * kDegToRad;
  const auto covered = [&](const Vec3& direction) {
    const Vec3 side = Normalize(Cross(direction, std::abs(direction.y) < 0.9 ? Vec3{0.0, 1.0, 0.0}
                                                                            : Vec3{1.0, 0.0, 0.0}));
    const Vec3 other = Cross(direction, side);
    Vec3 ring[8];
    for (int k = 0; k < 8; ++k) {
      const double around = std::numbers::pi / 4.0 * k;
      const double c = std::cos(around) * std::sin(between);
      const double t = std::sin(around) * std::sin(between);
      ring[k] = Vec3{direction.x * std::cos(between) + side.x * c + other.x * t,
                     direction.y * std::cos(between) + side.y * c + other.y * t,
                     direction.z * std::cos(between) + side.z * c + other.z * t};
    }
    return std::any_of(seen.begin(), seen.end(), [&](const Seen& frame) {
      return images(frame, direction) &&
             std::all_of(std::begin(ring), std::end(ring),
                         [&](const Vec3& near) { return images(frame, near); });
    });
  };

  // The directions the plan answers for, as its own check asks them (ADR 0076): twenty thousand
  // spread over the sphere, about a degree and a half apart, of which a band answers only for the
  // elevations its rings aim at; and the horizon besides, since a band of only its horizon ring
  // answers for nothing else and no point of the spread lands there. The horizon's directions are
  // asked and weigh nothing in a gap's share of the sphere, which is the spread's to say.
  double highest = 0.0;
  for (const CoverageNode& node : plan.nodes) {
    highest = std::max(highest, std::abs(Direction(node.targetOrientation).y));
  }
  constexpr int kSamples = 20000;
  constexpr int kOnTheHorizon = 720;
  struct Open {
    Vec3 direction;
    bool weighs;
  };
  std::vector<Open> open;
  const double golden = std::numbers::pi * (3.0 - std::sqrt(5.0));
  for (int i = 0; i < kSamples; ++i) {
    const double y = 1.0 - 2.0 * (static_cast<double>(i) + 0.5) / kSamples;
    if (std::abs(y) > highest) continue;
    const double radius = std::sqrt(std::max(0.0, 1.0 - y * y));
    const Vec3 direction{std::cos(golden * i) * radius, y, std::sin(golden * i) * radius};
    if (!covered(direction)) open.push_back(Open{direction, true});
  }
  for (int i = 0; i < kOnTheHorizon; ++i) {
    const double azimuth = 2.0 * std::numbers::pi * (i + 0.5) / kOnTheHorizon;
    const Vec3 direction{std::sin(azimuth), 0.0, -std::cos(azimuth)};
    if (!covered(direction)) open.push_back(Open{direction, false});
  }
  // Highest first, so the directions within reach of one lie in a run of elevations either side of
  // it: a reach of r radians moves the height by at most r. Each target then asks only that run,
  // rather than every direction open, which made a narrow fill lens cost seconds.
  std::stable_sort(open.begin(), open.end(), [](const Open& a, const Open& b) {
    return a.direction.y > b.direction.y;
  });

  // Gathered into targets a fill frame each covers: from the first direction still open, aimed at
  // the middle of what is open within reach of it, taking what is within reach of that. A gap wider
  // than a frame comes back as several targets, and filling all of them closes it.
  const double reach = reachDeg * kDegToRad;
  const double withinReach = std::cos(reach);
  const auto run = [&](const Vec3& around) {
    const auto from = std::lower_bound(open.begin(), open.end(), around.y + reach,
                                       [](const Open& o, double y) { return o.direction.y > y; });
    const auto to = std::upper_bound(from, open.end(), around.y - reach,
                                     [](double y, const Open& o) { return y > o.direction.y; });
    return std::pair{static_cast<size_t>(from - open.begin()),
                     static_cast<size_t>(to - open.begin())};
  };
  std::vector<bool> taken(open.size(), false);
  std::vector<CoverageGap> gaps;
  for (size_t first = 0; first < open.size(); ++first) {
    if (taken[first]) continue;
    const Vec3 seed = open[first].direction;
    Vec3 sum{};
    const auto [seedFrom, seedTo] = run(seed);
    for (size_t i = seedFrom; i < seedTo; ++i) {
      const Vec3& d = open[i].direction;
      if (!taken[i] && Dot(d, seed) >= withinReach) {
        sum = Vec3{sum.x + d.x, sum.y + d.y, sum.z + d.z};
      }
    }
    Vec3 aim = Normalize(sum);
    if (!IsUsableVector(aim) || Dot(aim, seed) < withinReach) aim = seed;
    int members = 0;
    const auto [aimFrom, aimTo] = run(aim);
    for (size_t i = aimFrom; i < aimTo; ++i) {
      if (!taken[i] && Dot(open[i].direction, aim) >= withinReach) {
        taken[i] = true;
        if (open[i].weighs) ++members;
      }
    }
    CoverageGap gap;
    gap.target = FromAzimuthElevation(std::atan2(-aim.x, -aim.z) * kRadToDeg,
                                      std::asin(std::clamp(aim.y, -1.0, 1.0)) * kRadToDeg);
    gap.solidAngleFraction = static_cast<double>(members) / kSamples;
    gaps.push_back(gap);
  }
  return Ok(std::move(gaps));
}

}  // namespace sphanorama
