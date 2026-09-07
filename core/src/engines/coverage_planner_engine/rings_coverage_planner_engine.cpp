#include "engines/coverage_planner_engine/rings_coverage_planner_engine.h"

#include <algorithm>
#include <span>
#include <cmath>

#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr const char* kComponent = "RingsCoveragePlannerEngine";
constexpr double kRadToDeg = 57.29577951308232;
constexpr double kDegToRad = 0.017453292519943295;

// Below this the 1/cos(elevation) widening runs away, so a ring that close to a pole is served by
// a single cell — which is correct: at the pole every azimuth looks at the same place.
constexpr double kPolarCosineFloor = 0.08;

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

  const double advance = 1.0 - spec.overlapTarget;
  const double verticalStep = spec.verticalFovDeg * advance;
  const double horizontalStep = spec.horizontalFovDeg * advance;

  // A lens narrow enough to need more cells than this is not a capture anyone completes — at one
  // second a cell it is over a day of holding a phone still. The bound exists so the counts below
  // stay inside int, since converting an out-of-range double to int is undefined behaviour and
  // arrives here as a plan rather than as a diagnosable failure.
  constexpr double kMaxCells = 100000.0;
  const double approximateRings = 180.0 / verticalStep + 1.0;
  const double approximateCells = approximateRings * (360.0 / horizontalStep + 1.0);
  if (!std::isfinite(approximateCells) || approximateCells > kMaxCells) {
    return Err<CapturePlan>(StatusCode::InvalidArgument, kComponent,
                            "this lens and overlap would need more cells than a session can hold");
  }

  // Rings are laid symmetrically about the horizon and reach the poles, so that a sphere is
  // covered rather than a band. An odd count keeps one ring exactly on the horizon, which is
  // where most of the interesting content is and where the user starts.
  const int halfRings = static_cast<int>(std::ceil(90.0 / verticalStep));
  const int ringCount = 2 * halfRings + 1;
  const double elevationStep = 180.0 / (ringCount - 1);

  CapturePlan plan;
  plan.spec = spec;
  uint64_t nextId = 1;

  // Without the caps the plan covers a band around the horizon instead of a sphere. The limit is
  // the highest elevation a cell can aim at and still have its whole field of view below the
  // pole — dropping every ring above that, rather than just the two pole cells, is what makes
  // "no caps" mean a band rather than a sphere with two holes in it.
  const double elevationLimit = spec.coverPoles ? 90.0 : 90.0 - spec.verticalFovDeg / 2.0;

  for (int ring = 0; ring < ringCount; ++ring) {
    const double elevation = -90.0 + elevationStep * ring;
    if (std::abs(elevation) > elevationLimit) continue;

    // A ring's circumference shrinks by cos(elevation), so the azimuth step widens to keep the
    // on-sphere spacing constant. Without this the poles collect hundreds of redundant cells.
    const double cosine = std::max(std::cos(elevation * kDegToRad), kPolarCosineFloor);
    const int cells = std::max(1, static_cast<int>(std::ceil(360.0 * cosine / horizontalStep)));

    for (int cell = 0; cell < cells; ++cell) {
      CoverageNode node;
      node.id = NodeId{nextId++};
      node.targetOrientation =
          FromAzimuthElevation(360.0 * cell / cells, elevation);
      node.acceptanceConeDeg = spec.acceptanceConeDeg;
      node.ringIndex = ring;
      plan.nodes.push_back(node);
    }
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
    // A cone that is not a usable measurement puts no cell inside it — the same answer
    // `ICaptureSessionManager::ArmBurst` gives, and they have to agree or the reticle closes on a
    // cell that will not arm (see `ICoveragePlannerEngine`'s header). It is spelled to fail closed:
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
    // A plain `>`, matching `ArmBurst`, which is the other reading of this same number and whose
    // comment cites these two lines. `!(x <= y)` is the NaN-safe spelling and it is not doing any
    // work here: the two guards above have already refused a non-finite cone and a target that is
    // not a rotation, and `AngleBetweenDirections` cannot answer NaN for usable inputs — so a
    // reviewer swapped it in both files and every test stayed green, which is what a guard that
    // cannot decide anything looks like. Written as the thing it means, so the next reader does
    // not have to work out which NaN it is defending against.
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
  guidance.rollErrorDeg =
      RollBetween(current.orientation, nearest->targetOrientation) * kRadToDeg;
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

}  // namespace sphanorama
