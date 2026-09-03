// Logging is a utility, so it must stay cheap enough to call from the capture loop. Level
// filtering is the mechanism for that, and it is what these tests pin.
#include <gtest/gtest.h>

#include <sstream>

#include "utilities/logger.h"

namespace sphanorama {
namespace {

TEST(StreamLogger, WritesMessagesAtOrAboveItsLevel) {
  std::ostringstream out;
  StreamLogger logger(out, LogLevel::Info);
  logger.Log(LogLevel::Info, "test", "visible");
  logger.Log(LogLevel::Error, "test", "also visible");
  const std::string text = out.str();
  EXPECT_NE(text.find("visible"), std::string::npos);
  EXPECT_NE(text.find("also visible"), std::string::npos);
}

TEST(StreamLogger, DropsMessagesBelowItsLevel) {
  std::ostringstream out;
  StreamLogger logger(out, LogLevel::Warn);
  logger.Log(LogLevel::Debug, "test", "should not appear");
  logger.Log(LogLevel::Info, "test", "should not appear either");
  EXPECT_TRUE(out.str().empty());
}

TEST(StreamLogger, NamesTheComponentAndLevel) {
  std::ostringstream out;
  StreamLogger logger(out, LogLevel::Trace);
  logger.Log(LogLevel::Warn, "CaptureSessionManager", "cell rejected");
  const std::string text = out.str();
  EXPECT_NE(text.find("CaptureSessionManager"), std::string::npos);
  EXPECT_NE(text.find("WARN"), std::string::npos);
}

TEST(CollectingLogger, KeepsRecordsForAssertions) {
  CollectingLogger logger;
  logger.Log(LogLevel::Error, "PoseEngine", "sensor lost");
  ASSERT_EQ(logger.records().size(), 1u);
  EXPECT_EQ(logger.records()[0].level, LogLevel::Error);
  EXPECT_EQ(logger.records()[0].component, "PoseEngine");
  EXPECT_EQ(logger.records()[0].message, "sensor lost");
}

TEST(CollectingLogger, CanBeAskedWhetherSomethingWasLogged) {
  CollectingLogger logger;
  logger.Log(LogLevel::Warn, "FrameStore", "spilled 12 frames to OPFS");
  EXPECT_TRUE(logger.Contains(LogLevel::Warn, "spilled"));
  EXPECT_FALSE(logger.Contains(LogLevel::Error, "spilled"));
}

}  // namespace
}  // namespace sphanorama
