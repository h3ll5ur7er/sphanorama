#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <random>
#include <vector>

#include "sphanorama/types.h"

namespace sphanorama::test {

// Every coordinate of every match moved by Gaussian noise of `sigma` pixels. By Box-Muller over the
// engine's raw draws rather than `std::normal_distribution`, whose output the standard leaves to
// each library, so the same seed is the same noise on every toolchain. Kept inside frames of
// `width` by `height`, since no detector finds a keypoint past its frame's edge and `Refine`
// refuses a match that lies there (ADR 0082).
inline std::vector<PairwiseResult> WithNoise(std::vector<PairwiseResult> pairs, double sigma,
                                             uint32_t seed, int32_t width = 640,
                                             int32_t height = 480) {
  std::mt19937 draws(seed);
  const auto uniform = [&] { return (static_cast<double>(draws()) + 0.5) / 4294967296.0; };
  const auto gauss = [&] {
    const double radius = std::sqrt(-2.0 * std::log(uniform()));
    return static_cast<float>(sigma * radius * std::cos(2.0 * std::numbers::pi * uniform()));
  };
  for (PairwiseResult& pair : pairs) {
    for (PixelMatch& match : pair.inlierMatches) {
      const float w = static_cast<float>(width), h = static_cast<float>(height);
      match.ax = std::clamp(match.ax + gauss(), 0.0f, w);
      match.ay = std::clamp(match.ay + gauss(), 0.0f, h);
      match.bx = std::clamp(match.bx + gauss(), 0.0f, w);
      match.by = std::clamp(match.by + gauss(), 0.0f, h);
    }
  }
  return pairs;
}

}  // namespace sphanorama::test
