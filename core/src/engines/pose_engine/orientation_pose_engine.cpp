#include "engines/pose_engine/orientation_pose_engine.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>

#include "utilities/quaternion.h"

namespace sphanorama {
namespace {

constexpr const char* kComponent = "OrientationPoseEngine";

// Above this the device is being swung rather than aimed, and a burst fired here yields five
// blurred frames and no good one. Chosen as a starting point, not a measurement — it is a config
// key's worth of tuning once real captures exist.
constexpr double kUnusableRateRadPerSec = 2.0;

double Magnitude(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

Vec3 Subtract(const Vec3& a, const Vec3& b) { return Vec3{a.x - b.x, a.y - b.y, a.z - b.z}; }

Vec3 Scaled(const Vec3& v, double s) { return Vec3{v.x * s, v.y * s, v.z * s}; }

// Whether this sample's rate is a measurement rather than a zeroed field nobody filled in.
//
// Asked of the sample rather than of the session's capability. A capability is a claim about the
// platform and the two can disagree: a DeviceMotionEvent that fires with a null rotationRate is a
// device that has the API and is reporting nothing, and a capability answered before the first
// sample arrived cannot know either way. Where they disagree the sample is the one that knows
// (ADR 0025).
//
// Both flags are *claims* about what the platform put in the sample, not guarantees about what it
// put there. A port can set `hasOrientation` beside a zero or NaN quaternion, and the value that
// reaches the estimate then is neither refused nor marked unknown: `Normalize` answers a
// degenerate rotation with `Quat{}`, which is the identity — an ordinary attitude pointing
// straight ahead, at a direction a capture plan names a cell at. Same for a non-finite rate, which
// integrates to a non-finite quaternion and collapses onto the identity by the same route.
//
// So the claim is checked against its own contents here, once, and every branch below reads these
// rather than the flags. `IsUsableRotation` and `IsUsableVector` are the questions "will
// `Normalize` derive something from this, or fall back?" and "is every component a number?".
bool Measured(const ImuSample& sample) {
  return sample.hasAngularVelocity && IsUsableVector(sample.angularVelocity);
}

bool Attitude(const ImuSample& sample) {
  return sample.hasOrientation && IsUsableRotation(sample.orientation);
}

// How the disagreement between the gyroscope's prediction and the absolute reading is split: part
// of it corrects the estimate now, part of it is charged to the gyroscope's zero offset. The pair
// is what makes this a complementary filter rather than a choice between two sensors — the
// attitude decides where the estimate ends up, the gyroscope decides how it gets there, and
// neither has to be trusted in the band where it is bad.
//
// **Both are times, not per-sample fractions, and that is deliberate.** A gain expressed as "this
// fraction of the error, every sample" behaves differently on a 60 Hz browser stream and a 200 Hz
// native one — the same filter would smooth four times harder on the faster device, which is the
// device that needed it least. Expressed as a time constant, the elapsed time in each sample does
// the work and the sensor's rate stops being a tuning parameter nobody chose.
//
// The values are starting points rather than measurements, the same standing as
// kUnusableRateRadPerSec above, and they become config keys once there are real captures to tune
// against. What they were chosen against is the shape of the failure at either extreme: correct
// faster and noise in the reading's tilt reaches the reticle, which then shivers on a target it is
// already holding; correct slower and the estimate ignores a device that has genuinely turned.
//
// The offset is learned over minutes, for tilt and heading alike, because a gyroscope's offset
// changes over minutes and anything faster learns something else. Learned in half a second, it
// follows a tilt disagreement that goes round — noise, or a sway read a little late — and the
// offsets it learns, each at right angles to an up that is itself going round, add up along the
// phone's vertical to one about up that the gyroscope does not have: 23.5° of heading from a
// reading whose gravity circles half a degree (ADR 0074). Tilt has no need of the speed: an
// offset not yet learned stands only a correction time's worth away, a tenth of a degree at
// 0.02 rad/s.
constexpr double kCorrectionSeconds = 0.1;
constexpr double kBiasSeconds = 50.0;

// Heading is the one part of a platform's attitude that can be wrong for seconds on end: its
// fusion pulls towards a magnetic field that steel and wiring indoors have bent, and a phone held
// still watches the reticle creep. Gravity does not wander like that, so only the turn about the
// world's up is trusted this slowly, and the gyroscope carries it in between (ADR 0074).
//
// The offset about up is learned from it too, because gravity cannot see it and on a phone that
// keeps its tilt nothing else would. `kBiasSeconds` is five times this, which keeps the heading's
// loop from ringing — it overshoots a step once, by about a tenth — and is slow enough that a creep
// teaches the offset almost nothing.
//
// A starting point on the same footing as the two above. Across the two-second dwell that fires a
// burst it lets through under a tenth of a creep (ADR 0074).
constexpr double kHeadingCorrectionSeconds = 10.0;

// The longest gap a prediction is made across. A browser's sensors report every 16 ms or so, and
// the adapter already refuses a rate more than 200 ms from its attitude; a gap longer than this is
// a stall — a hidden tab, a capture loop so far behind that the adapter's buffer dropped samples,
// or the main thread the sensor events are delivered on — and not an interval anybody measured.
constexpr double kLongestPredictionSeconds = 0.25;

// The largest disagreement about gravity a reading is fused across; past it the reading is not a
// measurement error (see where it is used). A real tilt the gyroscope missed is a few degrees.
constexpr double kLargestTiltRadians = 30.0 * std::numbers::pi / 180.0;

// How far from an exact quarter- or half-turn about the camera's axis a disagreement may be and
// still be a page turn: the phone is near still when the platform turns the page, so the reading's
// own error is most of it. A starting point, like the other two thresholds.
constexpr double kPageTurnToleranceRadians = 10.0 * std::numbers::pi / 180.0;

// The rotation from `from` to `to` as an axis-angle vector in `from`'s own frame, which is the
// frame the gyroscope's rates are already in — so the disagreement can be subtracted from a rate
// without any change of basis.
//
// Taken along the short way round: a quaternion and its negation are the same rotation, and the
// arithmetic below would otherwise correct a one-degree error by turning 359 degrees.
Vec3 RotationBetween(const Quat& from, const Quat& to) {
  Quat error = Multiply(Conjugate(from), to);
  if (error.w < 0.0) {
    error = Quat{-error.w, -error.x, -error.y, -error.z};
  }
  const double axis = std::sqrt(error.x * error.x + error.y * error.y + error.z * error.z);
  if (axis < 1e-12) return Vec3{0.0, 0.0, 0.0};   // no rotation, and no axis to name it about
  const double radians = 2.0 * std::atan2(axis, error.w);
  const double scale = radians / axis;
  return Vec3{error.x * scale, error.y * scale, error.z * scale};
}

// Turns an orientation by a rate held for a time. Small-angle about the instantaneous axis, which
// is what a sensor-rate sample is.
Quat Turned(const Quat& orientation, const Vec3& rate, double seconds) {
  const double magnitude = Magnitude(rate);
  if (magnitude <= 1e-9) return orientation;
  return Normalize(Multiply(orientation, FromAxisAngle(rate, magnitude * seconds)));
}

// The turn of the page a disagreement is, if it is one. Turning between portrait and landscape
// steps the frame the attitude and the rates are reported in by a quarter- or half-turn about the
// axis the camera looks down — the phone's Z — while the phone itself barely moves, so that shape
// is what tells one apart. Not gravity: near a pole that axis is nearly the world's up, and a page
// turn there moves gravity by less than kLargestTiltRadians.
std::optional<Quat> PageTurn(const Quat& disagreement) {
  for (const int quarters : {1, 2, 3}) {
    const Quat turn = FromAxisAngle(Vec3{0.0, 0.0, 1.0}, quarters * std::numbers::pi / 2.0);
    if (Magnitude(RotationBetween(turn, disagreement)) <= kPageTurnToleranceRadians) return turn;
  }
  return std::nullopt;
}

}  // namespace

Result<PoseState> OrientationPoseEngine::Initial(PoseMode mode, MotionCapability capability) {
  PoseState state;
  state.mode = mode;
  state.capability = capability;
  return Ok(state);
}

Result<PoseState> OrientationPoseEngine::Integrate(const PoseState& prior,
                                                   std::span<const ImuSample> samples) {
  PoseState state = prior;

  for (const ImuSample& sample : samples) {
    // Whether there is a gyroscope reading worth fusing this attitude with. Per sample rather
    // than per session: a stream can carry rates on some samples and not others, and a zeroed
    // angularVelocity means "not measured" wherever nothing filled it in. Blending a reading with
    // a gyroscope that is not there would only add lag to the one signal there is, which is why
    // every platform reporting an attitude alone behaves exactly as it did before this existed.
    const bool fusing = Measured(sample);
    // Two questions, and they were one flag. `elapsed` is whether there is an interval to integrate
    // a rate over — a clock, which the first measuring sample of a stream sets whether or not it
    // estimated anything. `predictable` is whether there is an *estimate* worth predicting forward
    // and correcting back from, which is a stronger thing: dead reckoning can start from the
    // identity, and a complementary filter cannot, because it would blend the first real reading
    // with a guess nobody measured. At a 16 ms gap the correction share is about 15%, so getting
    // that wrong leaves the pose most of a turn from a reading it should simply have taken.
    const bool elapsed = state.observed && sample.timestampNs > state.pose.timestampNs;
    // `absolute`, not "something moved it". A prediction is only worth blending against a reading
    // when it descends from a reading: dead reckoning moves the orientation too, so a stream
    // opening with rate-only samples reached the first attitude holding an estimate integrated
    // from the identity it was born with — a direction nobody chose — and correcting a fraction of
    // the way towards the first real reading kept most of that arbitrary origin. Third route to
    // the same 25.5°.
    //
    // It narrows the branch rather than disabling it: `absolute` is set by both attitude branches,
    // so once a stream is anchored the filter runs normally, and it goes false only where the
    // estimate really has stopped descending from a reading.
    const bool predictable = state.absolute && elapsed;
    const double seconds =
        elapsed ? static_cast<double>(sample.timestampNs - state.pose.timestampNs) * 1e-9 : 0.0;
    // A rate is a measurement of an instant, and held across a gap nobody sampled it says nothing
    // about how the phone turned in it. A reading after one is a reading after a dropout.
    const bool spanned = seconds <= kLongestPredictionSeconds;
    const bool fused = Attitude(sample) && fusing && predictable && spanned;
    // Where the gyroscope says the device now points, when there is a prediction to make.
    const Quat predicted =
        fused ? Turned(state.pose.orientation, Subtract(sample.angularVelocity, state.gyroBias),
                       seconds)
              : state.pose.orientation;

    // A page turn, recognised on every path a reading takes — fused, after the stall a relayout
    // makes, or ahead of the rates — since the rates arrive in the turned frame whichever it is.
    // The offset learned in the old frame is turned into the new one; left where it was, it
    // points a quarter-turn wrong and is learned again over minutes.
    const std::optional<Quat> page =
        Attitude(sample) ? PageTurn(Multiply(Conjugate(predicted), Normalize(sample.orientation)))
                         : std::nullopt;
    if (page) state.gyroBias = Rotate(Conjugate(*page), state.gyroBias);

    if (Attitude(sample) && (!fused || page)) {
      // Ground truth, taken as it stands. With no gyroscope there is nothing to disagree with it,
      // and on the first sample there is no elapsed time to have predicted anything over.
      state.pose.orientation = Normalize(sample.orientation);
      state.absolute = true;
      state.anchored = true;
    } else if (Attitude(sample)) {
      // From the prediction, take part of the way back to where the reading says the device
      // points. The prediction carries the fast motion the reading is too slow and too noisy to
      // follow; the reading anchors the slow drift the gyroscope cannot see in itself.

      // Split about the world's up rather than the phone's own vertical: pitched up, most of a
      // compass error lies along the axis the camera looks down.
      //
      // As a twist and a swing, not by projecting a rotation vector onto up. The two agree for
      // small disagreements and part company for large ones: projected, a heading jump carries
      // part of a small tilt with it and turns the rest sideways, and the fast loop chasing that
      // charges the gyroscope for a tilt that is not there. The swing is exactly the disagreement
      // about where gravity points, whatever the heading.
      const Vec3 up = Rotate(Conjugate(predicted), Vec3{0.0, 1.0, 0.0});
      const Quat disagreement = Multiply(Conjugate(predicted), Normalize(sample.orientation));
      const double along = Dot(Vec3{disagreement.x, disagreement.y, disagreement.z}, up);
      const Quat twist = Normalize(Quat{disagreement.w, up.x * along, up.y * along, up.z * along});
      const Quat swing = Multiply(Conjugate(twist), disagreement);
      const Vec3 heading = RotationBetween(Quat{}, twist);
      const Vec3 tilt = RotationBetween(Quat{}, swing);

      // Gravity is the half of an attitude a platform measures well, so a reading whose gravity is
      // tens of degrees from the prediction's is not a measurement error: a real turn faster than
      // the rate held across a gap shorter than kLongestPredictionSeconds, or a reading broken
      // outright. Taken whole, like a reading after a gap; fused, the share of it leaning towards
      // the world's up would be corrected at the heading's speed and the rest charged to the
      // gyroscope. The offset stays where it is, since the frame the rates arrive in did not turn.
      if (Magnitude(tilt) > kLargestTiltRadians) {
        state.pose.orientation = Normalize(sample.orientation);
      } else {
        // Exponential rather than a fixed share, so that ten samples of 1 ms correct as much as one
        // sample of 10 ms and no more.
        const double share = 1.0 - std::exp(-seconds / kCorrectionSeconds);
        const double headingShare = 1.0 - std::exp(-seconds / kHeadingCorrectionSeconds);
        state.pose.orientation =
            Turned(Turned(predicted, heading, headingShare), tilt, share);

        // The same disagreement, charged to the gyroscope — but first turned back into the *rate*
        // error that must have produced it, which is what a zero offset is. `share` does that: a
        // standing disagreement is the rate error times the window it accumulated over, and
        // `seconds / share` is that window, so multiplying by `share` leaves a rate, and dividing
        // by kBiasSeconds integrates it with one time constant however fast the samples arrive.
        // Charging `seconds` instead is quadratic in the gap, and once ran a 0.02 rad/s offset to
        // 10 rad/s at one-second gaps. A gap here is at most kLongestPredictionSeconds, so one
        // observation moves the offset by at most half a percent of the rate error it saw.
        //
        // No stillness detector, and none needed: what accumulates is the part of the error that
        // keeps pointing the same way. A device that is really turning produces a prediction the
        // reading agrees with, so there is nothing to charge.
        const double charge = share / kBiasSeconds;
        const double headingCharge = headingShare / kBiasSeconds;
        state.gyroBias = Subtract(state.gyroBias, Scaled(tilt, charge));
        state.gyroBias = Subtract(state.gyroBias, Scaled(heading, headingCharge));
      }
      state.absolute = true;
      // Anchored here as well as in the branch above, because both are branches that fold in a
      // reading and that is what anchoring is.
      //
      // It was briefly absent, on a proof that had gone stale under my own hand: the argument was
      // that this branch is reached only when `predictable` held and `predictable` requires the
      // flag — true when `predictable` was `state.estimated && elapsed`, and false since it became
      // `state.absolute && elapsed` one round later. Deleting the line left the suite green
      // because every caller in this repository reaches here with both flags set, which is an
      // invariant nothing states and nothing asserts — and `Integrate`'s prior is a caller's
      // value, not this engine's. A prior with `absolute` and not `anchored` ran the whole
      // complementary filter, moved 4.6 degrees from a real reading, and came back at confidence
      // zero: the contract's word for "nothing estimated this".
      state.anchored = true;
    } else if (elapsed && fusing) {
      // Dead reckoning, where nothing corrects the offset at all: an offset left in the rate
      // integrates straight into the answer.
      //
      // Gated on a measured rate, and the gate is not decorative: a sample with
      // neither an attitude nor a measured rate carries a zero-filled `angularVelocity`, so
      // subtracting a learned offset from it and integrating the result would turn "nothing was
      // reported" into a rotation backwards at the offset's own rate. A sample that reports
      // nothing should move nothing.
      const Vec3 rate = Subtract(sample.angularVelocity, state.gyroBias);
      if (Magnitude(rate) > 1e-9) {
        state.pose.orientation = Turned(state.pose.orientation, rate, seconds);
        // No anchor here, and that is the point of the flag. Integrating a rate says how far the
        // device has turned and nothing about where it started, so a stream that has never carried
        // an attitude is turning away from an identity nobody chose. `anchored` stays false, the
        // confidence below stays zero, and every rule that asks "is there an aim" gets the honest
        // answer for the whole stream rather than for its first sample only.
        // Dead reckoning from here on. Leaving the flag set would keep reporting an integrated
        // pose with the confidence of a measured one, and the drift would be invisible.
        state.absolute = false;
      }
      // Across a gap the rate says nothing about the turn either, still or not, so the estimate
      // stops descending from the reading before it whatever the rate integrated to.
      if (!spanned) state.absolute = false;
    }
    // The clock only moves forward. A sample too old to have an interval of its own is too old to
    // become the interval for the next one: adopting its timestamp unconditionally refused it and
    // then fabricated a gap for its successor, so one stale sample in a 60 Hz stream handed the
    // following sample a window hundreds of times too long — and the bias learned over that window
    // is charged as though the device had really been turning for it.
    //
    // And only for a sample that measured something. One that carries neither an attitude nor a
    // rate says nothing about the interval before it, and timed from it, a reading after a gap
    // would be predicted across the few milliseconds since rather than the gap itself.
    // `observed` with it, or a silent first sample starts a clock it never set, and the first rate
    // after it is integrated across its whole timestamp.
    if (Attitude(sample) || Measured(sample)) {
      state.pose.timestampNs = std::max(state.pose.timestampNs, sample.timestampNs);
      state.observed = true;
    }
  }

  if (!samples.empty()) state.pose.angularVelocity = samples.back().angularVelocity;
  // Zero until an absolute reading has been folded in: a caller reading confidence 0 knows it is
  // holding a direction nobody measured, whether that is the identity the state was born with or
  // an integration away from it.
  //
  // Keyed on `anchored` rather than on `observed` — a sample can arrive and inform nothing, and
  // both used to come back at 0.5 — and rather than on "something moved it", which is the same
  // mistake one sample later: a gyroscope-only stream moves the orientation on its second sample
  // and has still never been told where it is pointing.
  state.pose.confidence = !state.anchored ? 0.0 : (state.absolute ? 1.0 : 0.5);
  return Ok(state);
}

Result<PoseSample> OrientationPoseEngine::Correct(const FrameRef&, const FrameRef&,
                                                   const PoseSample& prior) {
  PoseSample pose = prior;
  pose.visuallyCorrected = false;
  return Ok(pose);
}

Result<double> OrientationPoseEngine::Stability(std::span<const ImuSample> samples) {
  if (samples.empty()) {
    // Not "perfectly still": reporting stability for a dropout would let a burst fire blind.
    return Err<double>(StatusCode::FailedPrecondition, kComponent,
                       "no samples to judge stability from");
  }

  // Measured rates when a platform supplies them, differences between attitudes when it does not.
  //
  // The distinction is not academic. A browser with no gyroscope adapted reports an attitude and
  // no rates, so every sample it produces carries a zeroed angularVelocity standing in for "not
  // measured" — and reading those zeros as a measurement reported a phone mid-swing as perfectly
  // still. Stability is what gates firing a burst, so that is the one direction this must not be
  // wrong in.
  //
  // `hasAngularVelocity` is what tells the two apart. It used to be `hasOrientation`, on the
  // reasoning that a sample carrying an attitude came from a platform reporting no rates — true
  // until a platform reported both, at which point a phone swung between two attitudes that
  // happened to match came back perfectly still with the gyroscope in the same sample saying
  // otherwise (ADR 0025).
  // Per interval rather than per batch, which is the correction a mixed stream forced. Choosing
  // once for the whole batch meant a single measured rate — a zero one, from a device that was
  // still a moment ago — switched the attitude fallback off for every sample after it, so a batch
  // that opened still and then swept through ninety degrees came back perfectly still. Each gap
  // between samples is now judged by the better signal available for *that* gap.
  //
  // Differentiating filtered attitudes is noisier than a gyroscope in exactly the band that
  // matters, so it remains the fallback wherever a rate was measured, and the only evidence
  // wherever one was not.
  double peak = 0.0;
  bool measured = false;
  const ImuSample* previous = nullptr;

  for (const ImuSample& sample : samples) {
    if (Measured(sample)) {
      peak = std::max(peak, Magnitude(sample.angularVelocity));
      measured = true;
    } else if (previous != nullptr && Attitude(sample) &&
               sample.timestampNs > previous->timestampNs) {
      const double seconds =
          static_cast<double>(sample.timestampNs - previous->timestampNs) * 1e-9;
      peak = std::max(peak, AngleBetween(previous->orientation, sample.orientation) / seconds);
      measured = true;
    }
    // Whatever carried an attitude is what the next gap is measured from, whether or not its own
    // rate was the thing that judged it.
    if (Attitude(sample)) previous = &sample;
  }

  if (!measured) {
    // One attitude says where the phone is, not whether it is moving. Answering "perfectly still"
    // from it would let a burst fire mid-swing on the first frame after a dropout.
    return Err<double>(StatusCode::FailedPrecondition, kComponent,
                       "a single sample cannot show whether the device is moving");
  }
  return Ok(std::clamp(1.0 - peak / kUnusableRateRadPerSec, 0.0, 1.0));
}

}  // namespace sphanorama
