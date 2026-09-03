#include "utilities/diagnostics.h"

namespace sphanorama {

void RecordingDiagnostics::Timing(const char* component, const char* stage, double milliseconds) {
  timings_[Key(component, stage)].push_back(milliseconds);
}

void RecordingDiagnostics::Counter(const char* component, const char* name, int64_t delta) {
  counters_[Key(component, name)] += delta;
}

int64_t RecordingDiagnostics::CounterValue(std::string_view component,
                                           std::string_view name) const {
  const auto it = counters_.find(Key(std::string(component), std::string(name)));
  return it == counters_.end() ? 0 : it->second;
}

std::vector<double> RecordingDiagnostics::Timings(std::string_view component,
                                                  std::string_view stage) const {
  const auto it = timings_.find(Key(std::string(component), std::string(stage)));
  return it == timings_.end() ? std::vector<double>{} : it->second;
}

}  // namespace sphanorama
