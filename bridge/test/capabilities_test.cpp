// The capability probe decides whether the core runs threaded. Getting it wrong in the
// optimistic direction means a phone crashes on a SharedArrayBuffer that was never available.
#include <gtest/gtest.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "capabilities.h"
#include "module.h"

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

// The boundary describes its own layout so the JavaScript side never re-declares field order.
// A reordering on one side and not the other would read plausible nonsense rather than fail,
// which is the failure mode these tests exist to make impossible.
TEST(ProbeFieldNames, EveryFieldHasAName) {
  const int32_t count = sph_probe_field_count();
  ASSERT_GT(count, 0);
  for (int32_t i = 0; i < count; ++i) {
    const char* name = sph_probe_field_name(i);
    ASSERT_NE(name, nullptr) << "field " << i;
    EXPECT_GT(std::string(name).size(), 0u) << "field " << i;
  }
}

TEST(ProbeFieldNames, NamesAreDistinct) {
  std::set<std::string> names;
  for (int32_t i = 0; i < sph_probe_field_count(); ++i) {
    names.insert(sph_probe_field_name(i));
  }
  EXPECT_EQ(static_cast<int32_t>(names.size()), sph_probe_field_count());
}

TEST(ProbeFieldNames, OutOfRangeIndexReturnsNullRatherThanReadingPastTheTable) {
  EXPECT_EQ(sph_probe_field_name(-1), nullptr);
  EXPECT_EQ(sph_probe_field_name(sph_probe_field_count()), nullptr);
  EXPECT_EQ(sph_probe_field_name(9999), nullptr);
}

TEST(ProbeRuntimeAbi, WritesEveryFieldAndReportsSuccess) {
  std::vector<int32_t> out(static_cast<size_t>(sph_probe_field_count()), -1);
  ASSERT_EQ(sph_probe_runtime(4, 0, out.data()), 0);
  for (const int32_t value : out) EXPECT_NE(value, -1);
}

TEST(ProbeRuntimeAbi, RefusesANullBufferRatherThanWritingThroughIt) {
  EXPECT_NE(sph_probe_runtime(4, 0, nullptr), 0);
}

TEST(ProbeRuntimeAbi, FieldNamesMatchWhatTheProbeReports) {
  // Ties the self-described layout to the values, so a field added to one and not the other
  // fails here rather than in a browser.
  std::vector<int32_t> out(static_cast<size_t>(sph_probe_field_count()), 0);
  ASSERT_EQ(sph_probe_runtime(8, 0, out.data()), 0);

  std::map<std::string, int32_t> decoded;
  for (int32_t i = 0; i < sph_probe_field_count(); ++i) {
    decoded[sph_probe_field_name(i)] = out[static_cast<size_t>(i)];
  }
  const auto caps = ProbeRuntime(8, false);
  EXPECT_EQ(decoded.at("threads"), caps.threads ? 1 : 0);
  EXPECT_EQ(decoded.at("sharedMemory"), caps.sharedMemory ? 1 : 0);
  EXPECT_EQ(decoded.at("simd"), caps.simd ? 1 : 0);
  EXPECT_EQ(decoded.at("hardwareConcurrency"), caps.hardwareConcurrency);
}

}  // namespace
}  // namespace sphanorama::bridge
