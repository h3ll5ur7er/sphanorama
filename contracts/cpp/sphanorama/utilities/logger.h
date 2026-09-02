#pragma once
#include <string_view>
#include "sphanorama/types.h"

namespace sphanorama {

enum class LogLevel : uint8_t { Trace, Debug, Info, Warn, Error };

class ILogger {
 public:
  virtual ~ILogger() = default;
  virtual void Log(LogLevel, const char* component, std::string_view message) = 0;
};

}  // namespace sphanorama
