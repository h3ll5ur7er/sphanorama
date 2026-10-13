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
// saying "none". One rule for every door a pose comes into the capture by — `OfferFrame` refuses
// one, a restored session document keeps the frame and drops the claim, guidance reads the live
// pose with the claim dropped, and a burst is neither armed nor continued on one — and for
// `Refine`, so the capture cannot hold a pose the solve will refuse. The live pose needs its doors
// because the shipped pose engine keeps a rotation a rotation without making one: an engine that
// reports a zero orientation at full confidence is aimed as though it faced the identity
// (ADR 0065).
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

// How far `current` is rolled against `target` about its own viewing axis, in radians, signed and
// in (-pi, pi]: the twist left over once the shortest turn has carried the target's view onto the
// current one. Zero when that turn alone takes one to the other, and continuous as an angle —
// a half-turn of roll is where it wraps from pi to just above -pi — wherever the two look, except
// within about two millionths of a radian of looking opposite ways. At opposite every turn
// perpendicular to the view is a shortest one and each leaves a different roll, so there is no
// roll to report; inside that band zero is reported, as it is for anything that is not a rotation.
// Swapping the two negates it, except at a half-turn of roll exactly, which reads pi either way.
//
// **Continuous is not the same as steady near opposite.** No roll can be continuous over every
// pair of views — the opposite point is where this one gives — so the roll changes fast around it:
// a level phone circling the point behind a level cell reads two full turns of roll per circuit,
// and ten degrees from behind it can read ninety. `RollFromLevel` asks about a target looking where
// the phone looks, so it reaches none of that.
//
// Two *level* orientations are not always zero apart: the shortest turn between views at different
// azimuths away from the horizon tips the horizon as it goes, so a level phone fifteen degrees of
// azimuth from a level cell thirty degrees up reads about 7.5 degrees.
double RollBetween(const Quat& current, const Quat& target);

// How far `current` is rolled from level about its own view, in radians and with `RollBetween`'s
// range and sign: the roll against the orientation that looks the same way with the horizon flat.
// It depends on nothing but `current`, so a level phone reads zero wherever it points — which is
// what `RollBetween` against a fixed target cannot do far from that target.
//
// Level is undefined looking straight up or down, and near there this is as unsteady as
// `RollBetween` is near opposite views: a small turn of the view swings the azimuth, and the roll
// with it. Zero for what is not a rotation, which is `RollBetween`'s answer to one.
double RollFromLevel(const Quat& current);

}  // namespace sphanorama
