#pragma once
#include "sphanorama/types.h"

namespace sphanorama {

// Timings and counters, kept on-device. Feeds the bench reports and the in-app diagnostics panel;
// nothing here leaves the machine.
class IDiagnosticsSink {
 public:
  virtual ~IDiagnosticsSink() = default;
  virtual void Timing(const char* component, const char* stage, double milliseconds) = 0;
  virtual void Counter(const char* component, const char* name, int64_t delta) = 0;
};

}  // namespace sphanorama
