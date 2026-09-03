// Algorithm parameters live in config rather than constants so the bench can sweep them without a
// rebuild. That only works if a missing key is a normal, cheap outcome.
#include <gtest/gtest.h>

#include "utilities/config_store.h"

namespace sphanorama {
namespace {

TEST(MapConfigStore, ReturnsTheFallbackWhenAKeyIsAbsent) {
  MapConfigStore config;
  EXPECT_TRUE(config.Flag("missing", true));
  EXPECT_DOUBLE_EQ(config.Number("missing", 2.5), 2.5);
  EXPECT_EQ(config.Text("missing", "fallback"), "fallback");
}

TEST(MapConfigStore, ReturnsTheStoredValueWhenPresent) {
  MapConfigStore config;
  config.SetFlag("ghostAware", false);
  config.SetNumber("overlapTarget", 0.35);
  config.SetText("strategy", "geodesic");
  EXPECT_FALSE(config.Flag("ghostAware", true));
  EXPECT_DOUBLE_EQ(config.Number("overlapTarget", 0.30), 0.35);
  EXPECT_EQ(config.Text("strategy", "rings"), "geodesic");
}

TEST(MapConfigStore, LaterWritesReplaceEarlierOnes) {
  MapConfigStore config;
  config.SetNumber("k", 1.0);
  config.SetNumber("k", 2.0);
  EXPECT_DOUBLE_EQ(config.Number("k", 0.0), 2.0);
}

TEST(MapConfigStore, KeyspacesAreSeparate) {
  // A number and a flag may share a name without colliding; nothing forces callers to coordinate.
  MapConfigStore config;
  config.SetNumber("tier", 3.0);
  EXPECT_TRUE(config.Flag("tier", true));
}

TEST(MapConfigStore, TextViewsStayValidAsTheStoreGrows) {
  // Text returns a view, so the store has to own stable storage; a vector-backed store would
  // dangle here the moment it reallocated.
  MapConfigStore config;
  config.SetText("a", "first");
  const std::string_view view = config.Text("a", "");
  for (int i = 0; i < 256; ++i) config.SetText("k" + std::to_string(i), "x");
  EXPECT_EQ(view, "first");
}

}  // namespace
}  // namespace sphanorama
