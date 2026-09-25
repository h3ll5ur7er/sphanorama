#pragma once

#include <optional>
#include <string_view>

#include "sphanorama/types.h"

namespace sphanorama {

// Orientation maths shared by the coverage planner, the pose engine and registration.
//
// **The functions that answer a question about orientations are total**: degenerate input (a zero
// quaternion from an uninitialised sensor read, a zero axis) yields identity, or zero for the ones
// returning an angle, rather than NaN. NaN does not fail loudly — it propagates through a whole
// capture session and surfaces as a sphere that will not close.
//
// **`Multiply` is algebra and is deliberately outside that**, which is a correction: this paragraph
// said "every function here" and named a zero quaternion as its worked example, and
// `Multiply(Quat{0,0,0,0}, q)` is the zero quaternion. Substituting the identity there would be a
// lie about what the product is, and a worse failure than the one it prevents — the zero propagates
// to `IsUsableRotation`, which refuses it, whereas an identity is a perfectly ordinary rotation
// nothing downstream can question. Every `Multiply` under `core/src` bar one is wrapped in
// `Normalize`; the exception is `OrientationPoseEngine`'s `RotationBetween`, whose inputs are
// normalised behind an `IsUsableRotation` gate.
//
// **`Conjugate` is not algebra and does not belong in that sentence, which first said it did.** It
// normalises before it negates, so `Conjugate(Quat{0,0,0,0})` is the identity and
// `IsUsableRotation` of it is `true` — precisely the "worse failure" the paragraph above describes,
// in the function that paragraph was claiming did not have it. The claim was generalised from
// `Multiply`, the one counter-example in hand, without reading the specification sitting three
// lines above `Conjugate`'s own declaration.
//
// **And `Norm` is outside the promise in the other direction**, which the same correction missed:
// `Norm(Quat{NaN,0,0,0})` is NaN and `Norm({0,1e200,0,0})` is an infinity. Not a defect — it is the
// arithmetic `IsUsableRotation` is built on, and a `Norm` answering zero for a NaN would break that
// predicate. Named so the quantifier is a list rather than a gesture.

double Norm(const Quat& q);

// Returns identity for a degenerate quaternion rather than dividing by zero.
Quat Normalize(const Quat& q);

// Whether this quaternion describes a rotation at all.
//
// `Normalize` answers a degenerate one with `Quat{}`, and `Quat{}` is the *identity* — a perfectly
// ordinary rotation whose `Direction` is `(0,0,-1)`. That is the right answer for a rendering
// helper, which needs some rotation and cannot fail, and the wrong one for anything deciding
// whether an attitude was measured: it turns "I could not tell" into "pointing straight ahead",
// which is a direction a capture plan can name a cell at.
//
// So callers that are deciding rather than drawing ask this first. There is no way to ask
// `Normalize`'s answer afterwards, because the identity is also what a phone genuinely held level
// reports.
bool IsUsableRotation(const Quat& q);

// Why a pose sample is not one, or nothing if it is. No pose is spelled with `confidence` zero, and
// at zero the orientation is not read; any other confidence outside [0, 1], or an orientation that
// is not a rotation where the confidence claims one, is a defect upstream rather than a way of
// saying "none". One rule for every door a pose comes into the capture by from outside the core —
// `OfferFrame` refuses one, and a restored session document keeps the frame and drops the claim —
// and for `Refine`, so the capture cannot hold a pose the solve will refuse. A burst's pose comes
// from the pose engine: its confidence range is `PoseSample`'s contract, and its orientation is a
// rotation only because the manager starts from the shipped engine's `Initial` and advances only
// by its `Integrate`, which keeps a rotation a rotation without making one (ADR 0065).
std::optional<std::string_view> PoseSampleDefect(const PoseSample& pose);

// The norm `IsUsableRotation` tested, returned so the caller divides by the value that was tested
// and by nothing else. Empty exactly when `IsUsableRotation` is false.
//
// A caller that gates and then divides needs the norm once, and this saves it the second
// evaluation. It used to be the only safe way to divide, because two evaluations could round
// differently: `Norm` was written as a plain sum of squares, and under `clang -O3
// -ffp-contract=fast` one compiled copy fused its multiply-adds and another was vectorised and did
// not, so near either end of the admissible range the gate said yes and the divisor read zero or
// infinity. `Norm` now fuses by hand with `std::fma`, so every copy rounds the same way and a
// second evaluation through `Norm` agrees with this one — measured bit for bit on nine builds,
// native and wasm. A norm spelled out by hand elsewhere does not, and has to come through here.
std::optional<double> UsableNorm(const Quat& q);

// Whether this vector is a measurement — every component finite. Not whether it is non-zero: a
// stationary phone really does report a zero angular velocity, and that is a rate rather than a
// silence.
bool IsUsableVector(const Vec3& v);

// Identity for a degenerate *axis* — zero, or one whose length is not finite — and identity for a
// degenerate *angle*. Both halves, because `sin` and `cos` of a non-finite angle are NaN and the
// promise above is about the whole input rather than the interesting part of it.
Quat FromAxisAngle(const Vec3& axis, double radians);

// Rotation separating two orientations, in radians, in [0, pi]. Treats q and -q as the same
// orientation, because they are.
double AngleBetween(const Quat& a, const Quat& b);

Quat Multiply(const Quat& a, const Quat& b);

// The inverse rotation, for a unit quaternion.
Quat Conjugate(const Quat& q);

// A vector rotated by an orientation.
Vec3 Rotate(const Quat& q, const Vec3& v);

// Orientation looking at a point on the sphere. Azimuth turns about +Y from the forward axis;
// elevation lifts toward +Y. This is the convention the coverage plan is expressed in.
Quat FromAzimuthElevation(double azimuthDeg, double elevationDeg);

// The direction the device is looking: -Z rotated by the orientation, matching the convention
// the browser's DeviceOrientation reports against.
Vec3 Direction(const Quat& q);

double Dot(const Vec3& a, const Vec3& b);
Vec3 Cross(const Vec3& a, const Vec3& b);

// Returns the zero vector for a degenerate input rather than dividing by zero, so callers can
// test for it instead of propagating NaN through a whole session.
Vec3 Normalize(const Vec3& v);

// The angle between two directions, in radians, in [0, pi]. Unlike AngleBetween on orientations
// this ignores rotation about the axis — which is what "how far off am I aiming" means.
double AngleBetweenDirections(const Vec3& a, const Vec3& b);

// Rotation about the viewing axis separating two orientations, in radians, signed and in
// (-pi, pi]. Zero when both are held the same way up.
//
// **Meaningful only while the two look in roughly the same direction, and this is the whole of the
// promise.** What stood here — "Zero, too, when the two look in opposite directions, where roll has
// no meaning" — is false twice over, measured. Opposite directions give **180 degrees**, not zero,
// and the answer is responsive there rather than nonsense: roll the target 30 degrees about its own
// viewing axis and it reads **-150**. The zero arrives at *ninety* degrees of separation instead,
// which is a different configuration entirely.
//
// Worse, it is not a degeneracy of roll but of the method. The implementation projects the target's
// +X axis off the current viewing axis, and that projection collapses when the two happen to align
// — a fact about the target's roll, not about whether roll exists. At azimuth 90 a target rolled by
// 15 degrees reads **-90**, and azimuth 89 versus 91 at zero roll flips between -0.0000 and
// 180.0000. A two-degree change in aim, 180 degrees of answer.
//
// **Both signs above were published positive and are negative**, which is worth a sentence because
// of how: they were measured by rolling about the body's +Z axis, and `Direction` is -Z rotated by
// the orientation, so that is the *negative* viewing axis. The figures were right for what was
// computed and the sentence describing what was computed was wrong — and they were exactly the two
// this file published without asserting. They are asserted now.
//
// Left as it is rather than fixed here, because every caller asks it against the *nearest* cell,
// where the separation is small and the function is well behaved, and rewriting it is a change to
// shipped guidance rather than to this branch's subject. The fix is a swing-twist decomposition,
// which is defined and continuous everywhere except exactly antipodal.
double RollBetween(const Quat& current, const Quat& target);

}  // namespace sphanorama
