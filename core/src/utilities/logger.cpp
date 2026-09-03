#include "utilities/logger.h"

namespace sphanorama {
namespace {

const char* Name(LogLevel level) {
  switch (level) {
    case LogLevel::Trace: return "TRACE";
    case LogLevel::Debug: return "DEBUG";
    case LogLevel::Info:  return "INFO";
    case LogLevel::Warn:  return "WARN";
    case LogLevel::Error: return "ERROR";
  }
  return "?";
}

}  // namespace

StreamLogger::StreamLogger(std::ostream& out, LogLevel minimum) : out_(out), minimum_(minimum) {}

void StreamLogger::Log(LogLevel level, const char* component, std::string_view message) {
  if (level < minimum_) return;
  out_ << '[' << Name(level) << "] " << component << ": " << message << '\n';
}

void CollectingLogger::Log(LogLevel level, const char* component, std::string_view message) {
  records_.push_back(LogRecord{level, component, std::string(message)});
}

bool CollectingLogger::Contains(LogLevel level, std::string_view substring) const {
  for (const auto& record : records_) {
    if (record.level == level && record.message.find(substring) != std::string::npos) return true;
  }
  return false;
}

}  // namespace sphanorama
