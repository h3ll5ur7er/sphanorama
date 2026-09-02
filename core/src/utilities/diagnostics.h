#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sphanorama/utilities/diagnostics.h"

namespace sphanorama {

// Keeps every timing sample rather than a running average: per-stage timings are compared across
// runs and device classes, and an average hides exactly the variance that matters there.
class RecordingDiagnostics final : public IDiagnosticsSink {
 public:
  void Timing(const char* component, const char* stage, double milliseconds) override;
  void Counter(const char* component, const char* name, int64_t delta) override;

  int64_t CounterValue(std::string_view component, std::string_view name) const;
  std::vector<double> Timings(std::string_view component, std::string_view stage) const;

 private:
  using Key = std::pair<std::string, std::string>;

  std::map<Key, int64_t> counters_;
  std::map<Key, std::vector<double>> timings_;
};

}  // namespace sphanorama
