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

// Whether the platform measures angular velocity at all. Named rather than compared, because
// GyroAccelMag is GyroAccel with a magnetometer on top and an equality test silently excluded it
// — which would have handed the better-equipped device the worse behaviour, and is exactly the
// kind of thing that reads as correct until someone lists the enumerators. Ordering them and
// testing >= would work today and would break the first time a capability is added in the middle.
bool HasGyroscope(MotionCapability capability) {
  return capability == MotionCapability::GyroAccel ||
         capability == MotionCapability::GyroAccelMag;
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

  // Whether there is a gyroscope worth fusing with. Read off the capability rather than guessed
  // from the numbers, because that is what the capability is for: on an OrientationOnly stream
  // every sample carries a zeroed angularVelocity standing in for "not measured", and treating
  // those zeros as a measurement of stillness is the same mistake Stability had to be fixed for.
  //
  // It also keeps this exactly as it was on every platform that ships today. Blending a reading
  // with a gyroscope that does not exist would only add lag to the one signal there is.
  const bool fusing = HasGyroscope(state.capability);

  for (const ImuSample& sample : samples) {
    const bool advanced = state.observed && sample.timestampNs > state.pose.timestampNs;
    const double seconds =
        advanced ? static_cast<double>(sample.timestampNs - state.pose.timestampNs) * 1e-9 : 0.0;

    if (sample.hasOrientation && !(fusing && advanced)) {
      // Ground truth, taken as it stands. With no gyroscope there is nothing to disagree with it,
      // and on the first sample there is no elapsed time to have predicted anything over.
      state.pose.orientation = Normalize(sample.orientation);
      state.absolute = true;
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
      // The clamp is what makes it safe for any gap rather than merely for realistic ones. A
      // single observation may move the offset by at most the whole rate error it saw, so the
      // estimate cannot overshoot the truth and cannot oscillate around it — whatever the
      // capture loop does with its timestamps.
      //
      // No stillness detector, and none needed: what accumulates here is the part of the error
      // that keeps pointing the same way. Noise does not, and cancels. A device that is really
      // turning produces a prediction the reading agrees with, so there is nothing to charge.
      const double charge = std::min(share / kBiasSeconds, 1.0);
      state.gyroBias = Subtract(state.gyroBias, Vec3{error.x * charge, error.y * charge,
                                                      error.z * charge});
      state.absolute = true;
    } else if (advanced) {
      // Dead reckoning, and the only stretch where the bias above earns its keep: nothing is
      // correcting the estimate, so an offset left in the rate integrates straight into the
      // answer. Subtracted only where it was learned — on a stream with no gyroscope it is zero
      // and this is the arithmetic it always was.
      const Vec3 rate =
          fusing ? Subtract(sample.angularVelocity, state.gyroBias) : sample.angularVelocity;
      if (Magnitude(rate) > 1e-9) {
        state.pose.orientation = Turned(state.pose.orientation, rate, seconds);
        // Dead reckoning from here on. Leaving the flag set would keep reporting an integrated
        // pose with the confidence of a measured one, and the drift would be invisible.
        state.absolute = false;
      }
    }
    state.pose.timestampNs = sample.timestampNs;
    state.observed = true;
  }

  if (!samples.empty()) state.pose.angularVelocity = samples.back().angularVelocity;
  // Zero until something has actually been observed: a caller reading confidence 0 knows the
  // orientation is a default rather than an estimate.
  state.pose.confidence = !state.observed ? 0.0 : (state.absolute ? 1.0 : 0.5);
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
  // The distinction is not academic. The browser reports an attitude and no rates at all, so
  // every sample it produces carries a zeroed angularVelocity standing in for "not measured" —
  // and reading those zeros as a measurement reported a phone mid-swing as perfectly still.
  // Stability is what gates firing a burst, so that is the one direction this must not be wrong
  // in. `hasOrientation` is what tells the two kinds of sample apart (ADR 0015).
  double peak = 0.0;
  bool measured = false;

  for (const ImuSample& sample : samples) {
    if (!sample.hasOrientation) {
      peak = std::max(peak, Magnitude(sample.angularVelocity));
      measured = true;
    }
  }

  // Differentiating filtered attitudes is noisier than a gyroscope in exactly the band that
  // matters, so it is the fallback rather than the default.
  if (!measured) {
    const ImuSample* previous = nullptr;
    for (const ImuSample& sample : samples) {
      if (!sample.hasOrientation) continue;
      if (previous != nullptr && sample.timestampNs > previous->timestampNs) {
        const double seconds =
            static_cast<double>(sample.timestampNs - previous->timestampNs) * 1e-9;
        const double radians = AngleBetween(previous->orientation, sample.orientation);
        peak = std::max(peak, radians / seconds);
        measured = true;
      }
      previous = &sample;
    }
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
