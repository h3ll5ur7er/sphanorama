#pragma once

#include <numbers>

namespace sphanorama::test {

// How far apart two rotations may read and still be the same rotation.
//
// **Zero is not available.** `AngleBetween` is `2 * acos(|dot|)`, and for a unit quaternion `dot`
// with itself is one only to rounding: 1.09% of random unit quaternions give `AngleBetween(q, q)`
// non-zero, worst 3.4e-6 degrees, and the smallest non-zero value it can produce at all is
// 1.7075e-6 degrees. So `EXPECT_NEAR(<angle>, 0.0, 1e-9)` never asserted "within a billionth of a
// degree" — it asserted **bit identity plus rounding luck**, and the luck is a property of the
// build: under `clang -O3 -march=native -ffp-contract=fast` a solve whose two answers were
// bit-identical read 1.7075e-6 degrees apart and failed on correct code. The same shape fails a
// pre-branch scorer test under plain `gcc -O3 -march=native`, which contracts by default in C++.
//
// The value is `kSettledDeg`'s: 1e-5 degrees, 5.9 times the floor, chosen there for the same
// reason and bracketed there from both sides. A solver whose stopping rule is 1e-5 degrees cannot
// honestly be asked to agree with anything more closely, and nothing a sabotage produces is measured
// in millionths of a degree — every one this branch has run produced degrees.
//
// One constant rather than a literal at fifty sites, because the literal was copied to fifty sites
// and every copy carried the same wrong assumption.
inline constexpr double kSameRotationDeg = 1e-5;
inline constexpr double kSameRotationRad = kSameRotationDeg * std::numbers::pi / 180.0;

}  // namespace sphanorama::test
