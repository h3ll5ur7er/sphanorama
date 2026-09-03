// Diagnostics feed the bench reports and the in-app panel. Counters accumulate rather than
// overwrite, because the interesting numbers (frames spilled, tiles rebuilt) are totals.
#include <gtest/gtest.h>

#include "utilities/diagnostics.h"

namespace sphanorama {
namespace {

TEST(RecordingDiagnostics, AccumulatesCounters) {
  RecordingDiagnostics diag;
  diag.Counter("FrameStore", "spills", 3);
  diag.Counter("FrameStore", "spills", 4);
  EXPECT_EQ(diag.CounterValue("FrameStore", "spills"), 7);
}

TEST(RecordingDiagnostics, KeepsCountersSeparatePerComponent) {
  RecordingDiagnostics diag;
  diag.Counter("FrameStore", "spills", 3);
  diag.Counter("ProjectStore", "spills", 1);
  EXPECT_EQ(diag.CounterValue("FrameStore", "spills"), 3);
  EXPECT_EQ(diag.CounterValue("ProjectStore", "spills"), 1);
}

TEST(RecordingDiagnostics, UnknownCounterReadsAsZero) {
  RecordingDiagnostics diag;
  EXPECT_EQ(diag.CounterValue("nobody", "nothing"), 0);
}

TEST(RecordingDiagnostics, KeepsEveryTimingSampleRatherThanOnlyTheLast) {
  // Per-stage timings are compared across runs and device classes, so a single overwritten
  // value would hide exactly the variance we care about.
  RecordingDiagnostics diag;
  diag.Timing("RegistrationEngine", "features", 12.0);
  diag.Timing("RegistrationEngine", "features", 18.0);
  const auto samples = diag.Timings("RegistrationEngine", "features");
  ASSERT_EQ(samples.size(), 2u);
  EXPECT_DOUBLE_EQ(samples[0], 12.0);
  EXPECT_DOUBLE_EQ(samples[1], 18.0);
}

}  // namespace
}  // namespace sphanorama
