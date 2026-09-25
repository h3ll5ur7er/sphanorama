#pragma once
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

#include "sphanorama/types.h"

namespace sphanorama::test {

// Every coordinate of every match moved by Gaussian noise of `sigma` pixels. By Box-Muller over the
// engine's raw draws rather than `std::normal_distribution`, whose output the standard leaves to
// each library, so the same seed is the same noise on every toolchain.
inline std::vector<PairwiseResult> WithNoise(std::vector<PairwiseResult> pairs, double sigma,
                                             uint32_t seed) {
  std::mt19937 draws(seed);
  const auto uniform = [&] { return (static_cast<double>(draws()) + 0.5) / 4294967296.0; };
  const auto gauss = [&] {
    const double radius = std::sqrt(-2.0 * std::log(uniform()));
    return static_cast<float>(sigma * radius * std::cos(2.0 * std::numbers::pi * uniform()));
  };
  for (PairwiseResult& pair : pairs) {
    for (PixelMatch& match : pair.inlierMatches) {
      match.ax += gauss();
      match.ay += gauss();
      match.bx += gauss();
      match.by += gauss();
    }
  }
  return pairs;
}

}  // namespace sphanorama::test
