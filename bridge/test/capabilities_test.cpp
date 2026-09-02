// The capability probe decides whether the core runs threaded. Getting it wrong in the
// optimistic direction means a phone crashes on a SharedArrayBuffer that was never available.
#include <gtest/gtest.h>

#include "capabilities.h"

namespace sphanorama::bridge {
namespace {

TEST(ProbeRuntime, ReportsNoThreadsWithoutCrossOriginIsolation) {
  // This is the GitHub Pages case: the build may support threads, the host cannot serve the
  // COOP/COEP headers, and SharedArrayBuffer is unavailable no matter what the binary wants.
  const auto caps = ProbeRuntime(8, /*crossOriginIsolated=*/false);
  EXPECT_FALSE(caps.threads);
  EXPECT_FALSE(caps.sharedMemory);
  EXPECT_EQ(caps.hardwareConcurrency, 0);
}

TEST(ProbeRuntime, ReportsNoThreadsOnASingleCoreDevice) {
  const auto caps = ProbeRuntime(1, /*crossOriginIsolated=*/true);
  EXPECT_FALSE(caps.threads);
}

TEST(ProbeRuntime, AnswersFromTheBuildItIsIn) {
  const auto caps = ProbeRuntime(16, /*crossOriginIsolated=*/true);
#if defined(__EMSCRIPTEN_PTHREADS__)
  EXPECT_TRUE(caps.threads);
  EXPECT_EQ(caps.hardwareConcurrency, 16);
#else
  // The native build has no Emscripten pthreads; claiming threads here would mean the probe
  // reports what the host offers rather than what this binary can use.
  EXPECT_FALSE(caps.threads);
  EXPECT_EQ(caps.hardwareConcurrency, 0);
#endif
}

TEST(ProbeRuntime, ConcurrencyIsZeroWheneverThreadsAreUnavailable) {
  // Callers size thread pools from this. A non-zero count with threads disabled would spawn
  // workers that cannot exist.
  for (const bool isolated : {false, true}) {
    const auto caps = ProbeRuntime(4, isolated);
    if (!caps.threads) {
      EXPECT_EQ(caps.hardwareConcurrency, 0);
    }
  }
}

}  // namespace
}  // namespace sphanorama::bridge
