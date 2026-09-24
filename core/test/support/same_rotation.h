#pragma once

#include <numbers>

namespace sphanorama::test {

// How far apart two rotations may read and still be the same rotation.
//
// **Zero is not available.** `AngleBetween` is `2 * acos(|dot|)`, and for a unit quaternion `dot`
// with itself is one only to rounding. The smallest non-zero value it can produce at all is
// `2 * acos(1 - 2^-53)` = 1.7075e-6 degrees, and that is the build-independent fact. How often a
// quaternion misses zero against itself is the build's: over a million random unit quaternions per
// seed, 1.07% to 1.10% under `gcc -O0` and under `clang -O3 -march=native -ffp-contract=fast`, and
// 2.74% to 2.79% under `gcc -O3 -march=native`. The worst a million draws usually reach is
// 3.4151e-6 degrees (`dot` three ulps under one); one seed reached 3.8182e-6 (four ulps), so that
// is the typical worst and not a bound.
//
// So `EXPECT_NEAR(<angle>, 0.0, 1e-9)` never asserted "within a billionth of a degree" — it
// asserted **bit identity plus rounding luck**, and the luck is a property of the build: under
// `clang -O3 -march=native -ffp-contract=fast` a solve whose two answers were bit-identical read
// 1.7075e-6 degrees apart and failed on correct code. The same shape failed a pre-branch scorer
// test under plain `gcc -O3 -march=native`, which contracts by default in C++ even under
// `-std=c++20` — 98 fused multiply-adds from `quaternion.cpp`, counted — and misses zero against
// itself 2.7% of the time there, as above.
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
