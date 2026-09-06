#include "engines/pose_engine/orientation_pose_engine.h"

#include <algorithm>
#include <cmath>

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

// Whether this sample's rate is a measurement rather than a zeroed field nobody filled in.
//
// Asked of the sample rather than of the session's capability. A capability is a claim about the
// platform and the two can disagree: a DeviceMotionEvent that fires with a null rotationRate is a
// device that has the API and is reporting nothing, and a capability answered before the first
// sample arrived cannot know either way. Where they disagree the sample is the one that knows
// (ADR 0025).
bool Measured(const ImuSample& sample) { return sample.hasAngularVelocity; }

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
// faster and magnetometer noise reaches the reticle, which then shivers on a target it is already
// holding; correct slower and the estimate ignores a device that has genuinely turned. Learn the
// offset faster and a real rotation is absorbed as one, so every turn after it lags; slower and
// the drift this exists to remove is still there when the attitude drops out.
constexpr double kCorrectionSeconds = 0.1;
constexpr double kBiasSeconds = 0.5;

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
    // a rate over — a clock, which the first sample of any stream sets whether or not it estimated
    // anything. `predictable` is whether there is an *estimate* worth predicting forward and
    // correcting back from, which is a stronger thing: dead reckoning can start from the identity,
    // and a complementary filter cannot, because it would blend the first real reading with a guess
    // nobody measured. At a 16 ms gap the correction share is about 15%, so getting that wrong
    // leaves the pose most of a turn from a reading it should simply have taken.
    const bool elapsed = state.observed && sample.timestampNs > state.pose.timestampNs;
    // `absolute`, not `estimated`. A prediction is only worth blending against a reading when it
    // descends from a reading: dead reckoning also sets `estimated`, so a stream opening with
    // rate-only samples reached the first attitude holding an estimate integrated from the identity
    // it was born with — a direction nobody chose — and correcting a fraction of the way towards
    // the first real reading kept most of that arbitrary origin. Third route to the same 25.5°.
    //
    // It narrows the branch rather than disabling it: `absolute` is set by both attitude branches,
    // so once a stream is anchored the filter runs normally, and it goes false only where the
    // estimate really has stopped descending from a reading.
    const bool predictable = state.absolute && elapsed;
    const double seconds =
        elapsed ? static_cast<double>(sample.timestampNs - state.pose.timestampNs) * 1e-9 : 0.0;

    if (sample.hasOrientation && !(fusing && predictable)) {
      // Ground truth, taken as it stands. With no gyroscope there is nothing to disagree with it,
      // and on the first sample there is no elapsed time to have predicted anything over.
      state.pose.orientation = Normalize(sample.orientation);
      state.absolute = true;
      state.estimated = true;
    } else if (sample.hasOrientation) {
      // Predict where the gyroscope says the device now points, then take part of the way back to
      // where the reading says it does. The prediction carries the fast motion the reading is too
      // slow and too noisy to follow; the reading anchors the slow drift the gyroscope cannot see
      // in itself.
      const Quat predicted =
          Turned(state.pose.orientation, Subtract(sample.angularVelocity, state.gyroBias), seconds);
      const Vec3 error = RotationBetween(predicted, Normalize(sample.orientation));

      // Exponential rather than a fixed share, so that ten samples of 1 ms correct as much as one
      // sample of 10 ms and no more.
      const double share = 1.0 - std::exp(-seconds / kCorrectionSeconds);
      state.pose.orientation = Turned(predicted, error, share);

      // The same disagreement, charged to the gyroscope — but first turned back into the *rate*
      // error that must have produced it, which is what a zero offset is.
      //
      // `share` is what does that conversion, and getting it wrong is how this went badly wrong
      // once. A standing disagreement is the rate error multiplied by the window it accumulated
      // over, and `seconds / share` is that window: it flattens out at the correction time for
      // samples closer together than one, and equals the gap itself for samples further apart.
      // So multiplying the disagreement by `share` divides out the window and leaves a rate,
      // and dividing by kBiasSeconds makes that an integral with a fixed time constant however
      // fast the samples arrive.
      //
      // Charging `seconds` instead — as this did — is quadratic in the gap, because the
      // disagreement already grows with it. At 60 Hz the two are within 10% of each other and at
      // one second apart the loop diverges: a 0.02 rad/s offset was learned as 10 rad/s over six
      // samples, and the dropout it was meant to protect drifted 90 degrees instead of one.
      //
      // A single observation may move the offset by at most the whole rate error it saw, so the
      // estimate cannot overshoot the truth and cannot oscillate around it — whatever the capture
      // loop does with its timestamps. That is a property of the expression below rather than of a
      // guard on it: the factor `share / max(kBiasSeconds, seconds)` peaks at 0.9933 (near a 0.28 s
      // gap) and is under 1 at every gap, because past the time constant the denominator grows with
      // `seconds` while `share` is already saturating at 1.
      //
      // It was a clamped expression until a reviewer removed the clamp and found the whole suite
      // still green — including the ten-gap sweep written for exactly this invariant. The clamp had
      // been load-bearing when the divisor was `kBiasSeconds` alone, where the factor really did
      // pass 1; `std::max` took the job away from it and the comment went on crediting it. A guard
      // that cannot fire reads as the thing keeping you safe, which is worse than no guard: the
      // next person to change the divisor would trust it.
      //
      // No stillness detector, and none needed: what accumulates here is the part of the error
      // that keeps pointing the same way. Noise does not, and cancels. A device that is really
      // turning produces a prediction the reading agrees with, so there is nothing to charge.
      // Divided by the *window*, not by the time constant alone. `error * share` is the rate error
      // (the disagreement with its accumulation window divided out); charging `share / kBiasSeconds`
      // therefore steps the offset by that rate times `seconds / kBiasSeconds`, which grows with the
      // gap — so the clamp below bounded the step by the whole *angle* of disagreement while its
      // comment claimed the whole rate error. The two differ by exactly that window.
      //
      // Measured before the fix, a still device reporting 0.02 rad/s: sign flip at a 2 s gap, and
      // −5.48 rad/s learned across twelve samples 10 s apart. `std::max` keeps the fast-sample
      // behaviour identical — under one time constant the window is the time constant — and makes
      // a long gap charge at most the rate error it actually saw, which is what the paragraph
      // above has always said it does.
      const double charge = share / std::max(kBiasSeconds, seconds);
      state.gyroBias = Subtract(state.gyroBias, Vec3{error.x * charge, error.y * charge,
                                                      error.z * charge});
      state.absolute = true;
      // No `estimated` here, and that is provable rather than an omission: this branch is reached
      // only when `predictable` held, and `predictable` requires `state.estimated`. A write that
      // cannot be the thing that sets a flag is a line no test can hold — deleting it left the
      // whole suite green, which is how it was found.
    } else if (elapsed && fusing) {
      // Dead reckoning, and the only stretch where the bias above earns its keep: nothing is
      // correcting the estimate, so an offset left in the rate integrates straight into the
      // answer.
      //
      // Gated on a measured rate, and the gate is not decorative: a sample with
      // neither an attitude nor a measured rate carries a zero-filled `angularVelocity`, so
      // subtracting a learned offset from it and integrating the result would turn "nothing was
      // reported" into a rotation backwards at the offset's own rate. A sample that reports
      // nothing should move nothing.
      const Vec3 rate = Subtract(sample.angularVelocity, state.gyroBias);
      if (Magnitude(rate) > 1e-9) {
        state.pose.orientation = Turned(state.pose.orientation, rate, seconds);
        state.estimated = true;
        // Dead reckoning from here on. Leaving the flag set would keep reporting an integrated
        // pose with the confidence of a measured one, and the drift would be invisible.
        state.absolute = false;
      }
    }
    // The clock only moves forward. A sample too old to have an interval of its own is too old to
    // become the interval for the next one: adopting its timestamp unconditionally refused it and
    // then fabricated a gap for its successor, so one stale sample in a 60 Hz stream handed the
    // following sample a window hundreds of times too long — and the bias learned over that window
    // is charged as though the device had really been turning for it.
    state.pose.timestampNs = std::max(state.pose.timestampNs, sample.timestampNs);
    // A sample arrived, which is what the next one's elapsed time is measured from. Whether it
    // *moved* anything is `estimated`, set by each branch above that did.
    state.observed = true;
  }

  if (!samples.empty()) state.pose.angularVelocity = samples.back().angularVelocity;
  // Zero until something has actually moved the orientation: a caller reading confidence 0 knows
  // it is holding a default rather than an estimate. Keyed on `estimated` rather than on
  // `observed`, because a sample can arrive and inform nothing — a blank one, or the first rate of
  // a stream, which has no interval to be integrated over yet. Both used to come back at 0.5.
  state.pose.confidence = !state.estimated ? 0.0 : (state.absolute ? 1.0 : 0.5);
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
    } else if (previous != nullptr && sample.hasOrientation &&
               sample.timestampNs > previous->timestampNs) {
      const double seconds =
          static_cast<double>(sample.timestampNs - previous->timestampNs) * 1e-9;
      peak = std::max(peak, AngleBetween(previous->orientation, sample.orientation) / seconds);
      measured = true;
    }
    // Whatever carried an attitude is what the next gap is measured from, whether or not its own
    // rate was the thing that judged it.
    if (sample.hasOrientation) previous = &sample;
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
