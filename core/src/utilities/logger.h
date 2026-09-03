#pragma once

#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "sphanorama/utilities/logger.h"

namespace sphanorama {

// Level filtering happens here rather than at call sites so that logging stays cheap enough to
// call from the capture loop without every caller guarding it.
class StreamLogger final : public ILogger {
 public:
  StreamLogger(std::ostream& out, LogLevel minimum);

  void Log(LogLevel, const char* component, std::string_view message) override;

 private:
  std::ostream& out_;
  LogLevel minimum_;
};

struct LogRecord {
  LogLevel level{};
  std::string component;
  std::string message;
};

// Test double: keeps records so a test can assert that a component reported what it should.
class CollectingLogger final : public ILogger {
 public:
  void Log(LogLevel, const char* component, std::string_view message) override;

  const std::vector<LogRecord>& records() const { return records_; }
  bool Contains(LogLevel level, std::string_view substring) const;

 private:
  std::vector<LogRecord> records_;
};

}  // namespace sphanorama
